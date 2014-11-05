/*
 * WebRTCConnection.cpp
 */

#include <cstdio>

#include "WebRtcConnection.h"
#include "DtlsTransport.h"
#include "SdesTransport.h"

#include "SdpInfo.h"
#include "rtp/RtpHeaders.h"

namespace erizo {
  DEFINE_LOGGER(WebRtcConnection, "WebRtcConnection");

  // time between receiver reports
  const unsigned int RTCP_MAX_TIME = 5000;
  const unsigned int RTCP_MIN_TIME = 1000;

  WebRtcConnection::WebRtcConnection(bool audioEnabled, bool videoEnabled, const std::string &stunServer, int stunPort, int minPort, int maxPort)
      : fec_receiver_(this) {
    ELOG_WARN("WebRtcConnection constructor stunserver %s stunPort %d minPort %d maxPort %d\n", stunServer.c_str(), stunPort, minPort, maxPort);
    sequenceNumberFIR_ = 0;
    bundle_ = false;
    this->setVideoSinkSSRC(55543);
    this->setAudioSinkSSRC(44444);
    videoSink_ = NULL;
    audioSink_ = NULL;
    fbSink_ = NULL;
    sourcefbSink_ = this;
    sinkfbSource_ = this;
    globalState_ = CONN_INITIAL;
    connEventListener_ = NULL;
    videoTransport_ = NULL;
    audioTransport_ = NULL;

    audioEnabled_ = audioEnabled;
    videoEnabled_ = videoEnabled;

    stunServer_ = stunServer;
    stunPort_ = stunPort;
    minPort_ = minPort;
    maxPort_ = maxPort;
    
    sending_ = true;
    send_Thread_ = boost::thread(&WebRtcConnection::sendLoop, this);

    rtpDataTracker_.lastSR = 0;
    rtpDataTracker_.lastFractionLost = 0;
    rtpDataTracker_.lastPacketLostCount= 0;
    rtpDataTracker_.currentPacketCount = 0;
    rtpDataTracker_.currentDataCount = 0;
    gettimeofday(&rtpDataTracker_.timestamp, NULL);
    rtpDataTracker_.allowedSize = 0;
    rtpDataTracker_.jitterEnhancer = 0;

    memset((void*)&rtcpData_, 0, sizeof(struct RtcpData));
    gettimeofday(&rtcpData_.timestamp, NULL);
    rtcpData_.timestamp.tv_sec -= 1;
  }

  WebRtcConnection::~WebRtcConnection() {
    ELOG_INFO("WebRtcConnection Destructor");
    sending_ = false;
    cond_.notify_one();
    send_Thread_.join();
    globalState_ = CONN_FINISHED;
    if (connEventListener_ != NULL){
      connEventListener_->notifyEvent(globalState_);
      connEventListener_ = NULL;
    }
    globalState_ = CONN_FINISHED;
    videoSink_ = NULL;
    audioSink_ = NULL;
    fbSink_ = NULL;
    delete videoTransport_;
    videoTransport_=NULL;
    delete audioTransport_;
    audioTransport_= NULL;
  }

  bool WebRtcConnection::init() {
    return true;
  }
  
  bool WebRtcConnection::setRemoteSdp(const std::string &sdp) {
    ELOG_DEBUG("Set Remote SDP %s", sdp.c_str());
    remoteSdp_.initWithSdp(sdp);
    //std::vector<CryptoInfo> crypto_remote = remoteSdp_.getCryptoInfos();
    int video = (remoteSdp_.videoSsrc==0?false:true);
    int audio = (remoteSdp_.audioSsrc==0?false:true);

    bundle_ = remoteSdp_.isBundle;
    ELOG_DEBUG("Is bundle? %d %d ", bundle_, true);
    localSdp_.getPayloadInfos() = remoteSdp_.getPayloadInfos();
    localSdp_.isBundle = bundle_;
    localSdp_.isRtcpMux = remoteSdp_.isRtcpMux;

    ELOG_DEBUG("Video %d videossrc %u Audio %d audio ssrc %u Bundle %d", video, remoteSdp_.videoSsrc, audio, remoteSdp_.audioSsrc,  bundle_);

    ELOG_DEBUG("Setting SSRC to localSdp %u", this->getVideoSinkSSRC());
    localSdp_.videoSsrc = this->getVideoSinkSSRC();
    localSdp_.audioSsrc = this->getAudioSinkSSRC();

    this->setVideoSourceSSRC(remoteSdp_.videoSsrc);
    this->thisStats_.setVideoSourceSSRC(this->getVideoSourceSSRC());
    this->setAudioSourceSSRC(remoteSdp_.audioSsrc);
    this->thisStats_.setAudioSourceSSRC(this->getAudioSourceSSRC());

    if (remoteSdp_.profile == SAVPF) {
      if (remoteSdp_.isFingerprint) {
        // DTLS-SRTP
        if (remoteSdp_.hasVideo) {
          videoTransport_ = new DtlsTransport(VIDEO_TYPE, "video", bundle_, remoteSdp_.isRtcpMux, this, stunServer_, stunPort_, minPort_, maxPort_);
        }
        if (!bundle_ && remoteSdp_.hasAudio) {
          audioTransport_ = new DtlsTransport(AUDIO_TYPE, "audio", bundle_, remoteSdp_.isRtcpMux, this, stunServer_, stunPort_, minPort_, maxPort_);
        }
      } else {
        // SDES
        std::vector<CryptoInfo> crypto_remote = remoteSdp_.getCryptoInfos();
        for (unsigned int it = 0; it < crypto_remote.size(); it++) {
          CryptoInfo cryptemp = crypto_remote[it];
          if (cryptemp.mediaType == VIDEO_TYPE
              && !cryptemp.cipherSuite.compare("AES_CM_128_HMAC_SHA1_80")) {
            videoTransport_ = new SdesTransport(VIDEO_TYPE, "video", bundle_, remoteSdp_.isRtcpMux, &cryptemp, this, stunServer_, stunPort_, minPort_, maxPort_);
          } else if (!bundle_ && cryptemp.mediaType == AUDIO_TYPE
              && !cryptemp.cipherSuite.compare("AES_CM_128_HMAC_SHA1_80")) {
            audioTransport_ = new SdesTransport(AUDIO_TYPE, "audio", bundle_, remoteSdp_.isRtcpMux, &cryptemp, this, stunServer_, stunPort_, minPort_, maxPort_);
          }
        }
      }
    }

    return true;
  }

  std::string WebRtcConnection::getLocalSdp() {
    boost::mutex::scoped_lock lock(updateStateMutex_);
    ELOG_DEBUG("Getting SDP");
    if (videoTransport_ != NULL) {
      videoTransport_->processLocalSdp(&localSdp_);
    }
    ELOG_DEBUG("Video SDP done.");
    if (!bundle_ && audioTransport_ != NULL) {
      audioTransport_->processLocalSdp(&localSdp_);
    }
    ELOG_DEBUG("Audio SDP done.");
    localSdp_.profile = remoteSdp_.profile;
    return localSdp_.getSdp();
  }

  int WebRtcConnection::deliverAudioData_(char* buf, int len) {
    writeSsrc(buf, len, this->getAudioSinkSSRC());
    if (bundle_){
      if (videoTransport_ != NULL) {
        if (audioEnabled_ == true) {
          this->queueData(0, buf, len, videoTransport_);
        }
      }
    } else if (audioTransport_ != NULL) {
      if (audioEnabled_ == true) {
        this->queueData(0, buf, len, audioTransport_);
      }
    }
    return len;
  }


  // This is called by our fec_ object when it recovers a packet.
  bool WebRtcConnection::OnRecoveredPacket(const uint8_t* rtp_packet, int rtp_packet_length) {
      this->queueData(0, (const char*) rtp_packet, rtp_packet_length, videoTransport_);
      return true;
  }

  int32_t WebRtcConnection::OnReceivedPayloadData(const uint8_t* /*payload_data*/, const uint16_t /*payload_size*/, const webrtc::WebRtcRTPHeader* /*rtp_header*/) {
      // Unused by WebRTC's FEC implementation; just something we have to implement.
      return 0;
  }

  int WebRtcConnection::deliverVideoData_(char* buf, int len) {
    writeSsrc(buf, len, this->getVideoSinkSSRC());
    if (videoTransport_ != NULL) {
      if (videoEnabled_ == true) {
          RtpHeader* h = reinterpret_cast<RtpHeader*>(buf);
          if (h->getPayloadType() == RED_90000_PT && !remoteSdp_.supportPayloadType(RED_90000_PT)) {
              // This is a RED/FEC payload, but our remote endpoint doesn't support that (most likely because it's firefox :/ )
              // Let's go ahead and run this through our fec receiver to convert it to raw VP8
              webrtc::RTPHeader hackyHeader;
              hackyHeader.headerLength = h->getHeaderLength();
              hackyHeader.sequenceNumber = h->getSeqNumber();
              // FEC copies memory, manages its own memory, including memory passed in callbacks (in the callback, be sure to memcpy out of webrtc's buffers
              if (fec_receiver_.AddReceivedRedPacket(hackyHeader, (const uint8_t*) buf, len, ULP_90000_PT) == 0) {
                  fec_receiver_.ProcessReceivedFec();
              }
            } else {
              this->queueData(0, buf, len, videoTransport_);
          }
      }
    }
    return len;
  }

  int WebRtcConnection::deliverFeedback_(char* buf, int len){
    // Check where to send the feedback
    RtcpHeader *chead = reinterpret_cast<RtcpHeader*> (buf);
//    ELOG_DEBUG("received Feedback type %u ssrc %u, sourcessrc %u", chead->packettype, chead->getSSRC(), chead->getSourceSSRC());
//    ELOG_DEBUG("RTCP-RR: fraction:%u;packets:%u;highestseq:%u/%u;jitter:%u;lastSR:%u", chead->fractionlost, chead->packetlostCount, chead->highestSequenceNumber/0x10000, chead->highestSequenceNumber%0x10000, chead->interarrivalJitter, chead->lastSR, chead->delaySinceLastSR);
    if (chead->getSourceSSRC() == this->getAudioSourceSSRC()) {
        writeSsrc(buf,len,this->getAudioSinkSSRC());
    } else {
        //modifyRtcpRR(buf, len);
        writeSsrc(buf,len,this->getVideoSinkSSRC());      
    }

    if (videoTransport_ != NULL) {
      this->queueData(0, buf, len, videoTransport_);
    }
    return len;
  }

  void WebRtcConnection::writeSsrc(char* buf, int len, unsigned int ssrc) {
    RtpHeader *head = reinterpret_cast<RtpHeader*> (buf);
    RtcpHeader *chead = reinterpret_cast<RtcpHeader*> (buf);
    //if it is RTCP we check it it is a compound packet
    if (chead->isRtcp()) {
        processRtcpHeaders(buf,len,ssrc);
    } else {
      head->ssrc=htonl(ssrc);
    }
  }

  void WebRtcConnection::onTransportData(char* buf, int len, Transport *transport) {
    if (audioSink_ == NULL && videoSink_ == NULL && fbSink_==NULL){
      return;
    }
    
    // PROCESS STATS
    if (this->statsListener_){ // if there is no listener we dont process stats
      RtpHeader *head = reinterpret_cast<RtpHeader*> (buf);
      if (head->payloadtype != RED_90000_PT && head->payloadtype != PCMU_8000_PT)     
        thisStats_.processRtcpPacket(buf, len);
    }
    RtcpHeader* chead = reinterpret_cast<RtcpHeader*>(buf);
    // DELIVER FEEDBACK (RR, FEEDBACK PACKETS)
    if (chead->isFeedback()){
        //modifyRtcpRR(buf, len);
      if (fbSink_ != NULL) {
        fbSink_->deliverFeedback(buf,len);
      }
    } else {
      // RTP or RTCP Sender Report
      if (bundle_) {
        // Check incoming SSRC
        RtpHeader *head = reinterpret_cast<RtpHeader*> (buf);
        RtcpHeader *chead = reinterpret_cast<RtcpHeader*> (buf);
        unsigned int recvSSRC;
        if (chead->packettype == RTCP_Sender_PT) { //Sender Report
          recvSSRC = chead->getSSRC();
        }else{
          recvSSRC = head->getSSRC();
        }
        // Deliver data
        if (recvSSRC==this->getVideoSourceSSRC() || recvSSRC==this->getVideoSinkSSRC()) {
          videoSink_->deliverVideoData(buf, len);
        } else if (recvSSRC==this->getAudioSourceSSRC() || recvSSRC==this->getAudioSinkSSRC()) {
          audioSink_->deliverAudioData(buf, len);
        } else {
          ELOG_ERROR("Unknown SSRC %u, localVideo %u, remoteVideo %u, ignoring", recvSSRC, this->getVideoSourceSSRC(), this->getVideoSinkSSRC());
        }
      } else if (transport->mediaType == AUDIO_TYPE) {
        if (audioSink_ != NULL) {
          RtpHeader *head = reinterpret_cast<RtpHeader*> (buf);
          // Firefox does not send SSRC in SDP
          if (this->getAudioSourceSSRC() == 0) {
            ELOG_DEBUG("Audio Source SSRC is %u", head->getSSRC());
            this->setAudioSourceSSRC(head->getSSRC());
            //this->updateState(TRANSPORT_READY, transport);
          }
          head->setSSRC(this->getAudioSinkSSRC());
          audioSink_->deliverAudioData(buf, len);
        }
      } else if (transport->mediaType == VIDEO_TYPE) {
        if (videoSink_ != NULL) {
          RtpHeader *head = reinterpret_cast<RtpHeader*> (buf);
          RtcpHeader *chead = reinterpret_cast<RtcpHeader*> (buf);
           // Firefox does not send SSRC in SDP
          if (this->getVideoSourceSSRC() == 0) {
            unsigned int recvSSRC;
            if (chead->packettype == RTCP_Sender_PT) { //Sender Report
              recvSSRC = chead->getSSRC();
            } else {
              recvSSRC = head->getSSRC();
            }
            ELOG_DEBUG("Video Source SSRC is %u", recvSSRC);
            this->setVideoSourceSSRC(recvSSRC);
            //this->updateState(TRANSPORT_READY, transport);
          }
          // change ssrc for RTP packets, don't touch here if RTCP
          if (chead->packettype != RTCP_Sender_PT) {
            head->setSSRC(this->getVideoSinkSSRC());
          }

          if (checkTransport(buf, len, &rtcpData_)) {
              videoSink_->deliverVideoData(buf, len);
          }

//          bool allowedToSend = measureRtpFlow(buf, len);
//          if (allowedToSend || chead->isRtcp()) {
//              videoSink_->deliverVideoData(buf, len);
//              rtpDataTracker_.allowedSize -= len;
//              rtpDataTracker_.desiredSize -= len;

//              if (rtpDataTracker_.desiredSize < 0) {
//                  rtpDataTracker_.jitterEnhancer += -rtpDataTracker_.desiredSize;
//              }
//          } else {
//              ELOG_DEBUG("Discarded RTP packet with size %i", len);
//              rtpDataTracker_.jitterEnhancer += 4;
//          }
        }
      }
    }
  }

  int WebRtcConnection::sendFirPacket() {
    ELOG_DEBUG("Generating FIR Packet");
    sequenceNumberFIR_++; // do not increase if repetition
    int pos = 0;
    uint8_t rtcpPacket[50];
    // add full intra request indicator
    uint8_t FMT = 4;
    rtcpPacket[pos++] = (uint8_t) 0x80 + FMT;
    rtcpPacket[pos++] = (uint8_t) 206;

    //Length of 4
    rtcpPacket[pos++] = (uint8_t) 0;
    rtcpPacket[pos++] = (uint8_t) (4);

    // Add our own SSRC
    uint32_t* ptr = reinterpret_cast<uint32_t*>(rtcpPacket + pos);
    ptr[0] = htonl(this->getVideoSinkSSRC());
    pos += 4;

    rtcpPacket[pos++] = (uint8_t) 0;
    rtcpPacket[pos++] = (uint8_t) 0;
    rtcpPacket[pos++] = (uint8_t) 0;
    rtcpPacket[pos++] = (uint8_t) 0;
    // Additional Feedback Control Information (FCI)
    uint32_t* ptr2 = reinterpret_cast<uint32_t*>(rtcpPacket + pos);
    ptr2[0] = htonl(this->getVideoSourceSSRC());
    pos += 4;

    rtcpPacket[pos++] = (uint8_t) (sequenceNumberFIR_);
    rtcpPacket[pos++] = (uint8_t) 0;
    rtcpPacket[pos++] = (uint8_t) 0;
    rtcpPacket[pos++] = (uint8_t) 0;

    if (videoTransport_ != NULL) {
      videoTransport_->write((char*)rtcpPacket, pos);
    }

    return pos;
  }

  void WebRtcConnection::updateState(TransportState state, Transport * transport) {
    boost::mutex::scoped_lock lock(updateStateMutex_);
    WebRTCEvent temp = globalState_;
    ELOG_INFO("Update Transport State %s to %d", transport->transport_name.c_str(), state);
    if (audioTransport_ == NULL && videoTransport_ == NULL) {
      return;
    }

    if (state == TRANSPORT_FAILED) {
      temp = CONN_FAILED;
      //globalState_ = CONN_FAILED;
      sending_ = false;
      ELOG_INFO("WebRtcConnection failed, stopping sending");
      cond_.notify_one();
      ELOG_INFO("WebRtcConnection failed, stopped sending");
    }

    
    if (globalState_ == CONN_FAILED) {
      // if current state is failed we don't use
      return;
    }

    if (state == TRANSPORT_STARTED &&
        (!remoteSdp_.hasAudio || (audioTransport_ != NULL && audioTransport_->getTransportState() == TRANSPORT_STARTED)) &&
        (!remoteSdp_.hasVideo || (videoTransport_ != NULL && videoTransport_->getTransportState() == TRANSPORT_STARTED))) {
      if (remoteSdp_.hasVideo) {
        videoTransport_->setRemoteCandidates(remoteSdp_.getCandidateInfos());
      }
      if (!bundle_ && remoteSdp_.hasAudio) {
        audioTransport_->setRemoteCandidates(remoteSdp_.getCandidateInfos());
      }
      temp = CONN_STARTED;
    }

    if (state == TRANSPORT_READY &&
        (!remoteSdp_.hasAudio || (audioTransport_ != NULL && audioTransport_->getTransportState() == TRANSPORT_READY)) &&
        (!remoteSdp_.hasVideo || (videoTransport_ != NULL && videoTransport_->getTransportState() == TRANSPORT_READY))) {
        // WebRTCConnection will be ready only when all channels are ready.
        temp = CONN_READY;
    }

    if (transport != NULL && transport == videoTransport_ && bundle_) {
      if (state == TRANSPORT_STARTED) {
        videoTransport_->setRemoteCandidates(remoteSdp_.getCandidateInfos());
        temp = CONN_STARTED;
      }
      if (state == TRANSPORT_READY) {
        temp = CONN_READY;
      }
    }

    if (temp == CONN_READY && globalState_ != temp) {
      ELOG_INFO("Ready to send and receive media");
    }

    if (audioTransport_ != NULL && videoTransport_ != NULL) {
      ELOG_INFO("%s - Update Transport State end, %d - %d, %d - %d, %d - %d", 
        transport->transport_name.c_str(),
        (int)audioTransport_->getTransportState(), 
        (int)videoTransport_->getTransportState(), 
        this->getAudioSourceSSRC(),
        this->getVideoSourceSSRC(),
        (int)temp, 
        (int)globalState_);
    }
    
    if (temp < 0) {
      return;
    }

    if (temp == globalState_ || (temp == CONN_STARTED && globalState_ == CONN_READY))
      return;

    globalState_ = temp;
    if (connEventListener_ != NULL)
      connEventListener_->notifyEvent(globalState_);
  }

  void WebRtcConnection::queueData(int comp, const char* buf, int length, Transport *transport) {
    if ((audioSink_ == NULL && videoSink_ == NULL && fbSink_==NULL) || !sending_) //we don't enqueue data if there is nothing to receive it
      return;
    boost::mutex::scoped_lock lock(receiveVideoMutex_);
    if (!sending_)
      return;
    if (comp == -1){
      sending_ = false;
      std::queue<dataPacket> empty;
      std::swap( sendQueue_, empty);
      dataPacket p_;
      p_.comp = -1;
      sendQueue_.push(p_);
      cond_.notify_one();
      return;
    }
    if (sendQueue_.size() < 1000) {
      dataPacket p_;
      memcpy(p_.data, buf, length);
      p_.comp = comp;
      p_.type = (transport->mediaType == VIDEO_TYPE) ? VIDEO_PACKET : AUDIO_PACKET;
      p_.length = length;
      sendQueue_.push(p_);
    }
    cond_.notify_one();
  }

  WebRTCEvent WebRtcConnection::getCurrentState() {
    return globalState_;
  }

  void WebRtcConnection::processRtcpHeaders(char* buf, int len, unsigned int ssrc){
    char* movingBuf = buf;
    int rtcpLength = 0;
    int totalLength = 0;
    do{
      movingBuf+=rtcpLength;
      RtcpHeader *chead= reinterpret_cast<RtcpHeader*>(movingBuf);
      rtcpLength= (ntohs(chead->length)+1)*4;      
      totalLength+= rtcpLength;
      chead->ssrc=htonl(ssrc);
      if (chead->packettype == RTCP_PS_Feedback_PT){
        FirHeader *thefir = reinterpret_cast<FirHeader*>(movingBuf);
        if (thefir->fmt == 4){ // It is a FIR Packet, we generate it
          //ELOG_DEBUG("Feedback FIR packet, changed source %u sourcessrc to %u fmt %d", ssrc, sourcessrc, thefir->fmt);
          this->sendFirPacket();
        }
      }
    } while(totalLength<len);
  }

  void WebRtcConnection::sendLoop() {
      while (sending_) {
          dataPacket p;
          {
              boost::unique_lock<boost::mutex> lock(receiveVideoMutex_);
              while (sendQueue_.size() == 0) {
                  cond_.wait(lock);
                  if (!sending_) {
                      return;
                  }
              }
              if(sendQueue_.front().comp ==-1){
                  sending_ =  false;
                  ELOG_DEBUG("Finishing send Thread, packet -1");
                  sendQueue_.pop();
                  return;
              }

              p = sendQueue_.front();
              sendQueue_.pop();
          }

          if (bundle_ || p.type == VIDEO_PACKET) {
              videoTransport_->write(p.data, p.length);
          } else {
              audioTransport_->write(p.data, p.length);
          }
      }
  }

  bool WebRtcConnection::measureRtpFlow(char *buf, int len) {
      ++rtpDataTracker_.currentPacketCount;
      rtpDataTracker_.currentDataCount += len;
//      ELOG_DEBUG("(%p)measureRptFlow => packets: %i; data: %i", (void*)this, rtpDataTracker_.currentPacketCount, rtpDataTracker_.currentDataCount);

      struct timeval now;
      gettimeofday(&now, NULL);
      unsigned int dt = (now.tv_sec - rtpDataTracker_.timestamp.tv_sec) * 1000 + (now.tv_usec - rtpDataTracker_.timestamp.tv_usec) / 1000;
//      if (dt > 10000) {
//          rtpDataTracker_.currentPacketCount = 1;
//          rtpDataTracker_.currentDataCount = len;
//          rtpDataTracker_.timestamp = now;
//      }


      // float dataspeed = rtpDataTracker_.currentDataCount / (float) dt;
      // kB/s = kbit/s / 8
      float maxspeed = 86.0f / 8.0f;//25;//12.5f;
      // ELOG_DEBUG("(%p)measureRtpFlow: dataspeed: %f = %i / %i", (void*)this, dataspeed, rtpDataTracker_.currentDataCount, dt);
      // return dataspeed <= maxspeed;

      rtpDataTracker_.allowedSize += dt * maxspeed;
      rtpDataTracker_.desiredSize += dt * maxspeed / 2;
      rtpDataTracker_.timestamp = now;
      ELOG_DEBUG("allowedSize: %f", rtpDataTracker_.allowedSize);
      return len <= rtpDataTracker_.allowedSize;
  }

  void WebRtcConnection::modifyRtcpRR(char *buf, int len) {
      RtcpHeader *chead = reinterpret_cast<RtcpHeader*> (buf);
      if (chead->packettype == RTCP_Receiver_PT && chead->length > 3) {
          uint32_t sr = chead->report.receiverReport.lastsr;
          if (sr > rtpDataTracker_.lastSR) {
              ELOG_DEBUG("(%p)ModifyRtcpRR: %i", (void*)this, sr);
//              struct timeval now;
//             gettimeofday(&now, NULL);
//              unsigned int dt = (now.tv_sec - rtpDataTracker_.timestamp.tv_sec) * 1000 + (now.tv_usec - rtpDataTracker_.timestamp.tv_usec) / 1000;
//              float dataspeed = (float)rtpDataTracker_.currentDataCount / (float) dt;
//              int fraction = 0;
//              if (dataspeed > 0) {
//                  float maxspeed = 12.5f; // 12.5 * 256 = 3200
//                  fraction = (int)(256 * (1 - maxspeed / dataspeed)); // 12.5 * 256 = 3200
//              }
//              ELOG_DEBUG("speed: %f = %i / %i => fraction = %i", dataspeed, rtpDataTracker_.currentDataCount, dt, fraction);
//              fraction = fraction < 0 ? 0 : (256 <= fraction ? 255 : fraction);
//              rtpDataTracker_.lastFractionLost = fraction;
//              rtpDataTracker_.lastPacketLostCount += (int)(rtpDataTracker_.currentPacketCount * ((float)fraction / 255.0f));
              rtpDataTracker_.lastSR = sr;
              int bonusJitter = rtpDataTracker_.jitterEnhancer;
              rtpDataTracker_.lastJitter = chead->report.receiverReport.jitter + bonusJitter;

//              rtpDataTracker_.currentDataCount = 0;
//              rtpDataTracker_.currentPacketCount = 0;
//              rtpDataTracker_.timestamp = now;
              rtpDataTracker_.jitterEnhancer = 0;
          }
          if (sr == rtpDataTracker_.lastSR) {
              logBuffer(buf, len);
//              chead->report.receiverReport.fractionlost = rtpDataTracker_.lastFractionLost;
//              chead->report.receiverReport.lost = rtpDataTracker_.lastPacketLostCount;
              chead->report.receiverReport.jitter = rtpDataTracker_.lastJitter;
              logBuffer(buf, len);
          }
      }
  }

  void WebRtcConnection::logBuffer(char *buf, int len)
    {
        char hex[] = "0123456789abcdefghjklmn";
        char *logstr = new char[len * 3 + 1];
        logstr[len * 3] = 0;
        for (int i = 0; i < len; ++i) {
            unsigned char current = (unsigned char)buf[i];
            logstr[i * 3 + 0] = hex[current / 16];
            logstr[i * 3 + 1] = hex[current % 16];
            logstr[i * 3 + 2] = ' ';
        }
        ELOG_DEBUG("LOGBUFFER: %s", logstr);
        delete[] logstr;
    }

    void WebRtcConnection::discardPacket(char *buf, int len) {
        if (rtpDataTracker_.tempBuf != NULL) {
            delete[] rtpDataTracker_.tempBuf;
        }
        rtpDataTracker_.tempBuf = new char[len];
        rtpDataTracker_.tempLen = len;
        memcpy((void*)rtpDataTracker_.tempBuf, (void*)buf, len);
    }

    void WebRtcConnection::checkPacket(char **p_buf, int *p_len) {
        if (rtpDataTracker_.tempBuf != NULL) {
            char* buf = rtpDataTracker_.tempBuf;
            int len = rtpDataTracker_.tempLen;

            rtpDataTracker_.tempBuf = NULL;
            discardPacket(*p_buf, *p_len);

            *p_buf = buf;
            *p_len = len;
        }
    }

    bool WebRtcConnection::checkTransport(char *buf, int len, struct RtcpData *pData) {
        // kB/s = kbit/s / 8
        const float maxspeed = 86.0f / 8.0f;//25;//12.5f;

        struct timeval now;
        gettimeofday(&now, NULL);
        unsigned int dt = (now.tv_sec - pData->timestamp.tv_sec) * 1000 + (now.tv_usec - pData->timestamp.tv_usec) / 1000;
if (pData->timestamp.tv_sec != 0) {        
        pData->allowedSize += dt * maxspeed * 2;
        pData->desiredSize += dt * maxspeed;
}
        pData->timestamp = now;

        RtcpHeader *chead = reinterpret_cast<RtcpHeader*> (buf);
        if (chead->packettype == RTCP_Sender_PT) {
            pData->lastSrReception = now;
            pData->lastSrTimestamp = (uint32_t)(chead->report.senderReport.ntptimestamp >> 16);
            ELOG_DEBUG("analysed SR - ntp timestamp: %x", pData->lastSrTimestamp);
            return true;
        } else if (chead->isRtcp()) {
            // unrecognized rtcp packet - ignore and send on
            return true;
        }

        // this is an rtp packet
        RtpHeader *head = reinterpret_cast<RtpHeader*>(buf);
        ++pData->packetCount;

        // nack list check
        for (int i = 0; i < pData->nackLen; ++i) {
            if (pData->nackList[i] == head->getSeqNumber()) {
                ELOG_DEBUG("Received NACK'd packet: %x", head->getSeqNumber());
                // nack'd? Remove from list and don't send on
                if (pData->nackLen == 1) {
                    pData->nackList = NULL;
                    pData->nackLen  = 0;
                    pData->requestRr = false;
                } else {
                    uint32_t *nackBuf = new uint32_t[pData->nackLen];
                    memcpy(nackBuf, pData->nackList, i * sizeof(uint32_t));
                    memcpy(nackBuf + i, pData->nackList + i + 1, (pData->nackLen - i - 1) * sizeof(uint32_t));
                    delete[] pData->nackList;
                    pData->nackLen = pData->nackLen - 1;
                    pData->nackList = nackBuf;
                }
                return false;
            }
        }

        if (pData->desiredSize < len) {
            ELOG_DEBUG("pretending to drop %x - len: %i vs %f", head->getSeqNumber(), len, pData->desiredSize);
            ++pData->lostPacketCount;

            // above what we want to send? remember for nack'ing
            uint32_t *nackBuf = new uint32_t[pData->nackLen + 1];

            if (pData->nackLen > 0) {
                memcpy(nackBuf, pData->nackList, pData->nackLen * sizeof(uint32_t));
            }
            nackBuf[pData->nackLen] = head->getSeqNumber();
            delete[] pData->nackList;
            pData->nackLen += 1;
            pData->nackList = nackBuf;

            pData->requestRr = true;
        } else {
            pData->desiredSize -= len;
            if (head->getSeqNumber() < pData->sequenceNumber) {
                ++pData->sequenceCycles;
            }
            pData->sequenceNumber = head->getSeqNumber();
            pData->ssrc = head->getSSRC();

            // ELOG_DEBUG("Valid RTP packet - SSRC: %x  SeqNum: %x", head->getSSRC(), head->getSeqNumber());
        }

        bool sendingAllowed = false;
        if (pData->allowedSize >= len) {
            // inside the hard size limit -> send on
            pData->allowedSize -= len;
            sendingAllowed = true;
        }

// ELOG_DEBUG("checkTransport - allowed: %f, desired: %f, size: %i", pData->allowedSize, pData->desiredSize, len);

        if (!pData->hasSentFirstRr) {
            pData->hasSentFirstRr = true;
            pData->lastRrSent = now;
            pData->lastRrSent.tv_sec -= RTCP_MAX_TIME / 2000;
            char packet[8];
            packet[0] = 0x80;
            packet[1] = 0xc9;
            packet[2] = 0;
            packet[3] = 0x01;
            uint32_t *ptr = reinterpret_cast<uint32_t*>(packet + 4);
            ptr[0] = htonl(getVideoSourceSSRC());
            deliverFeedback(packet, 8);
            ELOG_DEBUG("Sending initial RR");
            logBuffer(packet, 8);
            //pData->allowedSize = 0;
            //pData->desiredSize = 0;
        } else {
        unsigned int rtcpDt = (now.tv_sec - pData->lastRrSent.tv_sec) * 1000 + (now.tv_usec - pData->lastRrSent.tv_usec) / 1000;
        if (rtcpDt >= RTCP_MAX_TIME || (pData->requestRr && rtcpDt > RTCP_MIN_TIME)) {
            // create proper RR if time allows or requires
            sendReceiverReport(pData);
        }
        }

        return sendingAllowed;
    }

    void WebRtcConnection::sendReceiverReport(struct RtcpData *pData) {
        ELOG_DEBUG("sendReceiverReport(...)");
        int packetLen = 32;
        const int maxPacketLen = 128;
        uint8_t packet[maxPacketLen];
        packet[0] = 0x81; // header stuff, fixed
        packet[1] = 0xc9; // type: RR
        // 2,3: len 7
        packet[2] = 0;
        packet[3] = 7;
        // 4-7: local ssrc, ignored, will be overwritten
        packet[4] = 0x89;
        packet[5] = 0xab;
        packet[6] = 0xcd;
        packet[7] = 0xef;
        uint32_t *ptr = reinterpret_cast<uint32_t*>(packet + 8); // report block 1: ssrc
        ptr[0] = htonl(pData->ssrc); // 8-11: remote ssrc
        ptr[0] = htonl(getVideoSourceSSRC());

        float lostPercent = (float)pData->lostPacketCount / (float)pData->packetCount;
        pData->totalPacketsLost += pData->lostPacketCount;

        pData->lostPacketCount = 0;
        pData->packetCount = 0;

        ptr[1] = htonl(pData->totalPacketsLost); // 13-15: cumulative packets lost (12-15 used)
        uint8_t fractionLost = (uint8_t)(lostPercent < 0? 0 : (lostPercent >= 1? 255 : lostPercent * 255));
        packet[12] = fractionLost; // 12: fraction lost

        ptr[2] = htonl((pData->sequenceCycles << 16) | (pData->sequenceNumber & 0xffff)); // 16-19: ext seq num
        ptr[3] = htonl(50); // 20-23: jitter
        ptr[4] = htonl(pData->lastSrTimestamp); // 24-27: last sr

        struct timeval now;
        gettimeofday(&now, NULL);
        uint32_t delay = (now.tv_sec - pData->lastSrReception.tv_sec) * 0x10000 + (now.tv_usec - pData->lastSrReception.tv_usec) * 0x1000 / 1000000;
        ptr[5] = htonl(delay); // 28-31: delay since last sr


        // append NACK's
        if (pData->nackLen > 0) {
            packet[32] = 0x81; // header stuff
            packet[33] = 0xcd; // type: PSFTB/NACK
            // 34,35: len - not yet known, min: 3
            packet[34] = 0;
            packet[35] = 3;
            ptr = reinterpret_cast<uint32_t*>(packet + 36);
            // 36-39: local ssrc, ignored, will be overwritten
            ptr[1] = htonl(pData->ssrc); // 40-43: remote ssrc
            ptr[1] = htonl(getVideoSourceSSRC());

            uint16_t *shortPtr = reinterpret_cast<uint16_t*>(packet+44);

            int count = 0;
            uint16_t pid = pData->nackList[0];
            uint16_t blp = 0;
            const int countLimit = (maxPacketLen - 32 - 12) / 4; // 32: RR, 12: NACK overhead
            for(int i = 1; i < pData->nackLen; ++i) {
                uint16_t sqnum = pData->nackList[i];
                if (sqnum - pid <= 16) {
                    blp |= 1 << (sqnum - pid);
                } else {
                    shortPtr[count * 2 + 0] = htons(pid);
                    shortPtr[count * 2 + 1] = htons(blp);
                    if (count >= countLimit) break;
                    ++count;
                    pid = sqnum;
                    blp = 0;
                }
            }
            shortPtr[count * 2 + 0] = htons(pid);
            shortPtr[count * 2 + 1] = htons(blp);

            uint16_t curLen = 2 + count;
            shortPtr = reinterpret_cast<uint16_t*>(packet+34);
            shortPtr[0] = htons(curLen);

            packetLen += (curLen + 1) * 4;
        }

        ELOG_DEBUG("Generated RTCP");
        logBuffer((char*)packet, packetLen);
        deliverFeedback_((char*)packet, packetLen);
        logBuffer((char*)packet, packetLen);
        pData->lastRrSent = now;
    }
}
/* namespace erizo */
