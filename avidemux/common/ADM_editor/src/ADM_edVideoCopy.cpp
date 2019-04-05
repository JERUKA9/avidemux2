/***************************************************************************
    \file  ADM_edVideoCopy.cpp  
    \brief handle direct stream copy for video
    \author mean (c) 2002/2009 fixounet@free.fr

    

 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "ADM_cpp.h"
#include <math.h>
#include "ADM_default.h"
#include "ADM_edit.hxx"
#include "ADM_vidMisc.h"
#if 0
#define aprintf printf
#else
#define aprintf(...) {}// printf
#endif

static int warn_cnt=0;

/**
    \fn checkCutsAreOnIntra
    \brief In copy mode, if the cuts are not on intra we will run into trouble :
            * We include skipped ref frames: we will have DTS going back error
            * We skip them, we have borked video at cut points due to missing ref framesz
    \return true if everything ok
*/
bool ADM_Composer::checkCutsAreOnIntra(void)
{
    bool fail=false;
    int nbSeg=_segments.getNbSegments();

    ADMCompressedImage img;
    uint8_t *buffer=new uint8_t[1920*1080*3];
    img.data=buffer;
    ADM_info("Checking cuts start on keyframe..\n");
    for(int i=0;i<nbSeg;i++)
    {
        _SEGMENT *seg=_segments.getSegment(i);
        _VIDEOS *vid=_segments.getRefVideo(seg->_reference);
        vidHeader *demuxer=vid->_aviheader;

        if(false==switchToSegment(i,true))
        {
            fail=true;
            break;
        }
        if(false==demuxer->getFrame (vid->lastSentFrame,&img))
        {
            ADM_info("Cannot get 1st frame of segment %d\n",i);
            fail=true;
            break;
        }
        if(!(img.flags & AVI_KEY_FRAME))
        {
            ADM_warning("Segment %d does not start on a keyframe (%s)\n",i,ADM_us2plain(img.demuxerPts));
            fail=true;
            break;
        }
        // After a seg switch we are at the keyframe before or equal to where we want to go
        // if the dts do not match, it means we went back too much
        // When re-encoding, it's not a problem, it is when copying.
        ADM_info("seg %d: ref %d, refDTS=%" PRIu64"\n",i,seg->_reference,seg->_refStartDts);
        ADM_info("seg %d: ref %d, imgDTS=%" PRIu64"\n",i,seg->_reference,img.demuxerDts);
        if(!seg->_refStartDts && !seg->_reference)
        {
            ADM_info("Ignoring first seg (unreliable DTS)\n");
            
        }else
        if(img.demuxerDts!=ADM_NO_PTS && seg->_refStartDts!=ADM_NO_PTS && 
            img.demuxerDts!=seg->_refStartDts)
        {
            ADM_warning("Segment %d does not start on a known DTS (%s)\n",i,ADM_us2plain(img.demuxerDts));
            ADM_warning("expected (%s)\n",ADM_us2plain(seg->_refStartDts));
            fail=true;
            break;
        }
        ADM_info("Segment %d ok\n",i);
    }   
    delete [] buffer;
    buffer=NULL;
    if(fail) return false;
    return true;
}
/**
    \fn checkCutIsOnIntra
    \brief Allow to check if a particular delete operation results in a cut being not on an intra
*/
bool ADM_Composer::checkCutIsOnIntra(uint64_t time)
{
    bool fail=false;
    uint32_t segNo;
    uint64_t segTime;
    fail=!_segments.convertLinearTimeToSeg(time,&segNo,&segTime);
    if(fail)
        return true; // we can't do anything meaningful if we fail to convert time to segment

    ADMCompressedImage img;
    uint8_t *buffer=new uint8_t[1920*1080*3];
    img.data=buffer;
    ADM_info("Checking whether cut at %s is on a keyframe...\n",ADM_us2plain(time));

    _SEGMENT *seg=_segments.getSegment(segNo);
    _VIDEOS *vid=_segments.getRefVideo(seg->_reference);
    vidHeader *demuxer=vid->_aviheader;
    uint32_t oldSeg=_currentSegment;
    uint32_t oldFrame=vid->lastSentFrame;

    if(switchToSegment(segNo,true))
    {
        if(demuxer->getFrame(vid->lastSentFrame,&img))
        {
            if(!(img.flags & AVI_KEY_FRAME)) // always evaluates to false, img we've got is not the first frame of the segment
            {
                ADM_warning("Segment %d does not start on a keyframe (time in ref %s)\n",segNo,ADM_us2plain(img.demuxerPts));
                fail=true;
            }
        }else
        {
            ADM_info("Cannot get the 1st frame of segment %d\n",segNo);
        }
        if(!seg->_refStartDts && !seg->_reference)
        {
            ADM_info("Ignoring first seg (unreliable DTS)\n");
        }else if(img.demuxerDts!=ADM_NO_PTS && seg->_refStartDts!=ADM_NO_PTS && img.demuxerDts!=seg->_refStartDts)
        {
            ADM_warning("Segment %d does not start on a known DTS (%" PRIu64" us = %s)\n",segNo,img.demuxerDts,ADM_us2plain(img.demuxerDts));
            ADM_warning("expected: %" PRIu64" us = %s\n",seg->_refStartDts,ADM_us2plain(seg->_refStartDts));
            fail=true;
        }
        if(!fail)
            ADM_info("Segment %d is OK\n",segNo);
    }
    delete [] buffer;
    buffer=NULL;
    _currentSegment=oldSeg;
    vid->lastSentFrame=oldFrame;
    return !fail;
}
/**
     \fn bFrameDroppable
*/
static bool bFrameDroppable(uint32_t fcc)
{
    if(isH264Compatible(fcc)) 
      return false;
    if(isH265Compatible(fcc)) 
      return false;    
    return true;
}
/**
 * 
 * @param time
 * @param delay
 * \brief for x264/x265 we dont want to drop the non closed gop bframe(s)
 * They will be decoded after our first intra but shown before. 
 * They are needed because they might be used as reference for later frames.
 * 
 * For older codec (mpeg4 ASP, Mpeg1,...) these bframes can be just dropped.
 * 
 * This function tries to find out the maximum delay we must add
 * so that the overall PTS of these frames dont get negative.
 * @return 
 */
bool        ADM_Composer::getNonClosedGopDelay(uint64_t time,uint32_t *delay)
{
    aviInfo info;
    int found=-1;
    uint32_t startSegNo;
    uint64_t segTime;
    *delay=0;
    if(!_segments.convertLinearTimeToSeg(time,&startSegNo,&segTime))
    {
        ADM_warning("Cannot navigate to get nonclosedgop\n");
        return false;
    }

    for(uint32_t segNo=startSegNo; segNo < _segments.getNbSegments(); segNo++)
    {
        _SEGMENT *seg=_segments.getSegment(segNo);
        ADM_assert(seg);
        _VIDEOS *vid=_segments.getRefVideo(seg->_reference);
        uint64_t pts,dts;

        vid->_aviheader->getVideoInfo (&info);
        if(bFrameDroppable(info.fcc))
        {
            return true; // no need to add extra delay
        }
        // Look ahead to see if we have some late bframe in the past
        int nb=vid->_aviheader->getMainHeader ()->dwTotalFrames;
        if(segNo > startSegNo)
            segTime=0;
        uint64_t refTime=segTime+seg->_refStartTimeUs;
        found=-1;
        if(!refTime)
        {
            found=0;
            vid->_aviheader->getPtsDts(0,&pts,&dts);
        }else
        {
            for(int i=0;i<nb;i++)
            {
                vid->_aviheader->getPtsDts (i,&pts,&dts);
                if(pts==refTime)
                {
                    found=i;
                    break;
                }
            }
        }
        if(found==-1)
        {
            ADM_warning("Cannot find the frame for segment %" PRIu32"\n");
            continue;
        }
        for(int i=found+1;i<found+16;i++)
        {
            uint32_t flags;
            vid->_aviheader->getFlags(i,&flags);
            if(!(flags & AVI_B_FRAME))
            {
                ADM_info("Not a bframe, stopping (%d)\n",i-found);
                break;
            }
            vid->_aviheader->getPtsDts(i,&pts,&dts);
            if(pts<refTime)
            {
                ADM_info("frame %d is early \n",i);
                uint32_t delta=refTime-pts;
                if(delta>*delay) *delay=delta;
            }else
            {
                ADM_info("Pts delta = %d\n",(int)(pts-refTime));
            }
        }
    }
    if(found==-1)
        return false;
    ADM_info("Found maximum non closed gop delay = %d\n",*delay);
    return true;
}

/**
        \fn getCompressedPicture
        \brief bypass decoder and directly get the source image

    The dropBframe is as follow :
            0 : Dont drop b frame
            1 : Follow the next bframes
            2 : Drop


*/
bool        ADM_Composer::getCompressedPicture(uint64_t videoDelay,bool sanitize,ADMCompressedImage *img)
{
    uint64_t tail;
    //
    int64_t signedPts;
    int64_t signedDts;

#define MAX_EXTRA_DELAY 100000
#define CATCH_UP_RATE 5000
#define MAX_DESYNC_SCORE 20*100000
#define DESYNC_THRESHOLD 20000
#define ENOUGH 4

    if(totalExtraDelay>=CATCH_UP_RATE)
        totalExtraDelay-=CATCH_UP_RATE; // gradually reduce extra delay, CATCH_UP_RATE/1000 ms at a time

againGet:
    static uint32_t fn;
    fn++;
    _SEGMENT *seg=_segments.getSegment(_currentSegment);
    ADM_assert(seg);
    _VIDEOS *vid=_segments.getRefVideo(seg->_reference);
    ADM_assert(vid);
    vidHeader *demuxer=vid->_aviheader;
    ADM_assert(demuxer);

    // Prepare to deal with field-encoded streams
    uint64_t timeIncrement=vid->timeIncrementInUs;
    // Get next pic?
    if(false==demuxer->getFrame (vid->lastSentFrame,img))
    {
        ADM_info("Failed to get next frame for ref %" PRIu32"\n",seg->_reference);
        goto nextSeg;
    }
    aprintf("Got frame %d, PTS=%s ",vid->lastSentFrame,ADM_us2plain(img->demuxerPts));
    aprintf("DTS=%s\n",ADM_us2plain(img->demuxerDts));
    vid->lastSentFrame++;
    //
    aviInfo    info;
    vid->_aviheader->getVideoInfo (&info);
    //
    // after a segment switch, we may have some frames from "the past"
    // if the cut point is not a keyframe, drop them
    
    if(bFrameDroppable(info.fcc))
    {
        if(img->flags & AVI_B_FRAME)
        {
            if(seg->_dropBframes==_SEGMENT::ADM_DROPPING) 
            {
                ADM_warning("%" PRIu32" Dropping bframes\n",fn);
                goto againGet;
            }
        }else
        { // not a bframe
            switch(seg->_dropBframes)
            {
                case _SEGMENT::ADM_NO_DROP                : break;
                case _SEGMENT::ADM_DROPPING               : seg->_dropBframes=_SEGMENT::ADM_NO_DROP;break;
                case _SEGMENT::ADM_DROP_MAYBE_AFER_SWITCH : seg->_dropBframes=_SEGMENT::ADM_DROPPING;break;
                default: break;
            }
        }
    }
    if( img->demuxerDts!=ADM_NO_PTS)
    {
        bool drop=false;
        if(_currentSegment  && img->demuxerDts<seg->_refStartDts)
        {
            ADM_info("Frame %d is in the past for this segment (%s)",vid->lastSentFrame,ADM_us2plain(img->demuxerDts));
            ADM_info("vs refstartdts %s\n",ADM_us2plain(seg->_refStartDts));
            ADM_info(" dts=%llu, ref=%llu\n",img->demuxerDts,seg->_refStartDts);
            drop=true;
        }
        // Seeking is not accurate when cutting on non intra
        // we might have some frames that are clearly too early , even in seg0
        if(img->demuxerPts!=ADM_NO_PTS && bFrameDroppable(info.fcc))
        {
            if(img->demuxerPts+seg->_startTimeUs<seg->_refStartTimeUs)
            {
                ADM_info("Frame %d is in the past for this segment -bis (%s)",vid->lastSentFrame,ADM_us2plain(img->demuxerPts));
                ADM_info("vs refstartdts %s\n",ADM_us2plain(seg->_refStartTimeUs));
                ADM_info(" pts=%llu, startTime=%llu, _refStartTimeUs=%llu\n",img->demuxerPts,seg->_startTimeUs,seg->_refStartTimeUs);
                drop=true;
            }
        }
        if(drop)
            goto againGet;
        
    }

    // Need to switch seg ?
    tail=seg->_refStartTimeUs+seg->_durationUs;
    // Guess DTS
    //
//**
   // ADM_info("Frame : Flags :%X, DTS:%"PRId64" PTS=%"PRId64" nextDts=%"PRId64" tail=%"PRId64"\n",img->flags,img->demuxerDts/1000,img->demuxerPts/1000,_nextFrameDts,tail);
    if(img->demuxerDts!= ADM_NO_PTS && img->demuxerDts>=tail) 
    {
        std::string tailAsString=std::string(ADM_us2plain(tail));
        ADM_info("DTS is too late, switching segment (%s vs %s)\n",ADM_us2plain(img->demuxerDts),tailAsString.c_str());
        goto nextSeg;
    }
    if(img->demuxerPts!= ADM_NO_PTS && img->demuxerPts>=tail) 
    {
        std::string tailAsString=std::string(ADM_us2plain(tail));
        ADM_info("PTS is too late, switching segment (%s vs %s)\n",ADM_us2plain(img->demuxerPts),tailAsString.c_str());
        goto nextSeg;
    }
    
    // Since we rely on PTS for seeking, frame 0 might be at PTS 0, in that case the matching dts would be <0
    // so the caller can delay everything but recalibrate will clamp the value
    // so we use correctedDts so that the value is ok
    if(img->demuxerDts==ADM_NO_PTS)
    {
            aprintf("DTS unset for frame %" PRIu32"\n",vid->lastSentFrame-1);
            signedDts=ADM_NO_PTS;
    }else
    {
            signedDts=(int64_t)img->demuxerDts;
            signedDts+=totalExtraDelay;
            recalibrateSigned(&(signedDts),seg);
            aprintf("Signed Dts=%s ",ADM_us2plain(signedDts));
    }

    if(img->demuxerPts==ADM_NO_PTS)
    {
            aprintf("PTS unset for frame %" PRIu32"\n",vid->lastSentFrame-1);
            signedPts=ADM_NO_PTS;
    }else
    {
            signedPts=(int64_t)img->demuxerPts;
            signedPts+=totalExtraDelay;
            recalibrateSigned(&(signedPts),seg);
            aprintf("Signed Pts=%s\n",ADM_us2plain(signedPts));
    }
    // Halve timeIncrement for field-encoded MPEG-2 streams...
    if(isMpeg12Compatible(info.fcc) && img->flags & (AVI_STRUCTURE_TYPE_MASK+AVI_FIELD_STRUCTURE))
        timeIncrement/=2;
    // From here we are in linear time, guess DTS if missing...
    if(signedDts==ADM_NO_PTS)
    {
	// border case due to rounding we can have pts slighly above dts
	if(_nextFrameDts!=ADM_NO_PTS)
	{
            signedDts=_nextFrameDts;
            if(signedPts != ADM_NO_PTS && signedDts>signedPts)
		{
			// not sure it is correct. We may want to do it the other way around, i.e. bumping pts
			ADM_warning("Compensating for rounding error with PTS=%" PRId64"ms DTS=%" PRId64"ms \n",signedPts,signedDts);
                        signedPts=signedDts;
		}
	}
    }else
    {
// It means that the incoming image is earlier than the expected time.
// we add a bit of timeIncrement to compensate for rounding
        if(sanitize && _nextFrameDts!=ADM_NO_PTS)
        {
            if(_nextFrameDts>(signedDts+(int64_t)(timeIncrement/3)))
            {
                double delta=_nextFrameDts-signedDts;
                delta=fabs(delta);
                if((uint64_t)delta<MAX_EXTRA_DELAY && totalExtraDelay<MAX_EXTRA_DELAY)
                {
                    ADM_warning("Frame %d DTS is going back in time, delaying it for %" PRIu64" us\n",(int)vid->lastSentFrame,(uint64_t)delta);
                    signedDts=_nextFrameDts;
                    totalExtraDelay+=(uint64_t)delta;
                    ADM_info("total extra delay = %" PRIu64" us\n",totalExtraDelay);
                    signedPts+=totalExtraDelay;
                }else
                {
                    ADM_error("Frame %d DTS is going back in time: expected: %s : %d\n",
                          (int)vid->lastSentFrame,
                          ADM_us2plain(_nextFrameDts),
                          (_nextFrameDts));
                    ADM_error("and got %s : %d, timeIncrement=%d us, delta=%d\n",
                          ADM_us2plain(signedDts),
                          signedDts,(int)timeIncrement,
                          (int)delta);
                    uint64_t linearTime=img->demuxerPts-seg->_refStartTimeUs+seg->_startTimeUs;
                    char msg[512+1];
                    if(img->flags & AVI_KEY_FRAME)
                    {
                        snprintf(msg,512,QT_TRANSLATE_NOOP("adm",
                            "Decode time stamp (DTS) collision affecting a keyframe at %s detected.\n"
                            "Dropping a keyframe will result in severely corrupted video.\n"
                            "Proceed anyway?"),ADM_us2plain(linearTime));
                    }else
                    {
                        snprintf(msg,512,QT_TRANSLATE_NOOP("adm",
                            "Decode time stamp (DTS) collision affecting a frame at %s detected.\n"
                            "Dropping a frame may result in some video corruption.\n"
                            "Proceed anyway?"),ADM_us2plain(linearTime));
                    }
                    if(warn_cnt>=0)
                    {
                        if(!GUI_Question(msg))
                        {
                            warn_cnt=0;
                            desyncScore=0;
                            totalExtraDelay=0;
                            return false;
                        }else
                        {
                            warn_cnt++;
                        }
                    }
                    if(warn_cnt>ENOUGH)
                    { // warn ENOUGH+1 times in a row then suggest going silent
                        if(GUI_Question(QT_TRANSLATE_NOOP("adm","Do not warn again and drop frames silently while saving this video?")))
                            warn_cnt=-1; // Yes: don't warn
                        else
                            warn_cnt=0; // No: ask again after ENOUGH+1 more warnings
                    }

                    goto againGet;
                }
            }
        }
        _nextFrameDts=signedDts;
    }
    // Increase for next one
    if(ADM_NO_PTS!=_nextFrameDts)
        _nextFrameDts+=timeIncrement;
    // Check the DTS is not too late compared to next seg beginning...
    if(_currentSegment+1<_segments.getNbSegments() && img->demuxerDts!=ADM_NO_PTS)
    {
        _SEGMENT *nextSeg=_segments.getSegment(_currentSegment+1);
        int64_t nextDts=nextSeg->_startTimeUs+nextSeg->_refStartDts;
        if(nextDts<nextSeg->_refStartTimeUs)
        {
            ADM_warning("%" PRIu32" next DTS is negative %" PRIu64" %" PRIu64" ms\n",fn,nextDts,nextSeg->_refStartTimeUs);
        }else       
        {
            nextDts-=nextSeg->_refStartTimeUs;
            if(signedDts>=nextDts)
            {
                ADM_warning("%" PRIu32" have to switch segment, DTS limit reached %" PRIu64" %" PRIu64"\n",fn,img->demuxerDts/1000,nextDts/1000);
                goto nextSeg;
            }
        }


    }
// **
   // ADM_info("Frame after RECAL: Flags :%X, DTS:%"PRId64" PTS=%"PRId64" tail=%"PRId64"\n",img->flags,img->demuxerDts/1000,img->demuxerPts/1000,tail);
    {
    int64_t finalDts=ADM_NO_PTS;
    if(signedDts!=ADM_NO_PTS)
        finalDts=signedDts+(int64_t)videoDelay;
    if(finalDts!=ADM_NO_PTS && finalDts<0)
    {
        ADM_warning("Final DTS <0, dropping it \n");
        finalDts=0;
        goto againGet;
    }
    img->demuxerDts=finalDts;
    }
    if(signedPts!=ADM_NO_PTS)
        img->demuxerPts=signedPts+(int64_t)videoDelay;
    else
        img->demuxerPts=ADM_NO_PTS;
    aprintf("Final Pts=%s ",ADM_us2plain(img->demuxerPts));
    aprintf("Final Dts=%s ",ADM_us2plain(img->demuxerDts));    
    aprintf("Delay=%s\n",ADM_us2plain(videoDelay));
#if 0
    {
    std::string ptsAsString,dtsAsString,frameTypeAsString;
    ptsAsString=std::string(ADM_us2plain(img->demuxerPts));
    dtsAsString=std::string(ADM_us2plain(img->demuxerDts));
    frameTypeAsString="P";
    if(img->flags & AVI_KEY_FRAME) frameTypeAsString="I";
    if(img->flags & AVI_B_FRAME) frameTypeAsString="B";
    printf("Frame %" PRIu32", DTS=%s, PTS=%s, type: %s\n",vid->lastSentFrame-1,dtsAsString.c_str(),ptsAsString.c_str(),frameTypeAsString.c_str());
    }
#endif
    if(MAX_EXTRA_DELAY && totalExtraDelay>DESYNC_THRESHOLD && desyncScore>=0)
    {
        desyncScore+=(vid->timeIncrementInUs)*totalExtraDelay/MAX_EXTRA_DELAY;
        aprintf("Desync score = %" PRId64"\n",desyncScore);
    }
    if(desyncScore>MAX_DESYNC_SCORE)
    {
        uint64_t linearTime=img->demuxerPts-seg->_refStartTimeUs+seg->_startTimeUs;
        char msg[512+1];
        snprintf(msg,512,QT_TRANSLATE_NOOP("adm",
        "While saving, some video frames prior to %s had to be delayed, resulting in temporary loss of A/V sync. "
        "Would you like to continue nevertheless?"),ADM_us2plain(linearTime));
        if(!GUI_Question(msg))
        {
            desyncScore=0;
            totalExtraDelay=0;
            warn_cnt=0;
            return false;
        }
        desyncScore=-1; // ignore future desync
    }
    return true;

nextSeg:
    if(false==switchToNextSegment(true))
    {
        ADM_warning("Cannot update to new segment\n");
        totalExtraDelay=0;
        ADM_info("Accumulated desync score = %" PRId64"\n",desyncScore);
        desyncScore=0;
        warn_cnt=0;
        return false;
    }
    // Mark it as drop b frames...
    _SEGMENT *thisseg=_segments.getSegment(_currentSegment);
    thisseg->_dropBframes=_SEGMENT::ADM_DROP_MAYBE_AFER_SWITCH;
    ADM_info("Retrying for next segment\n");
    return getCompressedPicture(videoDelay,sanitize,img);
}

/**
        \fn     getDirectImage
        \brief  For DEBUG DO NOT USE!

*/
bool        ADM_Composer::getDirectImageForDebug(uint32_t frameNum,ADMCompressedImage *img)
{
  
    _SEGMENT *seg=_segments.getSegment(0);
    ADM_assert(seg);
    _VIDEOS *vid=_segments.getRefVideo(seg->_reference);
    ADM_assert(vid);
    vidHeader *demuxer=vid->_aviheader;
    ADM_assert(demuxer);

    // Get next pic?
    if(false==demuxer->getFrame (frameNum,img))
    {
        ADM_info("Failed to get next frame for ref %" PRIu32"\n",seg->_reference);
        return false;
    }
   return true;
}
