#ifndef WEBRTCCONNECTION_H_
#define WEBRTCCONNECTION_H_

#include <string>
#include <queue>
#include <boost/thread/mutex.hpp>
#include <boost/thread.hpp>
#include <sys/time.h>

#include "logger.h"
#include "SdpInfo.h"
#include "MediaDefinitions.h"
#include "Transport.h"
#include "Stats.h"
#include "rtp/webrtc/fec_receiver_impl.h"

namespace erizo {

class Transport;
class TransportListener;

/**
 * WebRTC Events
 */
enum WebRTCEvent {
  CONN_INITIAL = 101, CONN_STARTED = 102, CONN_READY = 103, CONN_FINISHED = 104, 
  CONN_FAILED = 500
};

class WebRtcConnectionEventListener {
public:
	virtual ~WebRtcConnectionEventListener() {
	}
	;
	virtual void notifyEvent(WebRTCEvent newEvent, const std::string& message="")=0;

};

class WebRtcConnectionStatsListener {
public:
	virtual ~WebRtcConnectionStatsListener() {
	}
	;
	virtual void notifyStats(const std::string& message)=0;
};
/**
 * A WebRTC Connection. This class represents a WebRTC Connection that can be established with other peers via a SDP negotiation
 * it comprises all the necessary Transport components.
 */
class WebRtcConnection: public MediaSink, public MediaSource, public FeedbackSink, public FeedbackSource, public TransportListener, public webrtc::RtpData {
	DECLARE_LOGGER();
public:
	/**
	 * Constructor.
	 * Constructs an empty WebRTCConnection without any configuration.
	 */
	WebRtcConnection(bool audioEnabled, bool videoEnabled, const std::string &stunServer, int stunPort, int minPort, int maxPort);
	/**
	 * Destructor.
	 */
	virtual ~WebRtcConnection();

	/**
	 * Inits the WebConnection by starting ICE Candidate Gathering.
	 * @return True if the candidates are gathered.
	 */
	bool init();
  void close();
	/**
	 * Sets the SDP of the remote peer.
	 * @param sdp The SDP.
	 * @return true if the SDP was received correctly.
	 */
	bool setRemoteSdp(const std::string &sdp);
	/**
	 * Obtains the local SDP.
	 * @return The SDP as a string.
	 */
	std::string getLocalSdp();

	/**
	 * Sends a FIR Packet (RFC 5104) asking for a keyframe
	 * @return the size of the data sent
	 */
	int sendFirPacket();
  
  /**
   * Sets the Event Listener for this WebRtcConnection
   */

	inline void setWebRtcConnectionEventListener(
			WebRtcConnectionEventListener* listener){
    this->connEventListener_ = listener;
  }
	
  /**
   * Sets the Stats Listener for this WebRtcConnection
   */
  inline void setWebRtcConnectionStatsListener(
			WebRtcConnectionStatsListener* listener){
    this->statsListener_ = listener;
    this->thisStats_.setPeriodicStats(STATS_INTERVAL, listener);
  }
	/**
	 * Gets the current state of the Ice Connection
	 * @return
	 */
	WebRTCEvent getCurrentState();

	void onTransportData(char* buf, int len, Transport *transport);

	void updateState(TransportState state, Transport * transport);

    void queueData(int comp, const char* data, int len, Transport *transport, packetType type);


    // webrtc::RtpHeader overrides.
    int32_t OnReceivedPayloadData(const uint8_t* payloadData, const uint16_t payloadSize,const webrtc::WebRtcRTPHeader* rtpHeader);
    bool OnRecoveredPacket(const uint8_t* packet, int packet_length);

    /**
     * set the limits for video bandwidth on this connection, soft will not drop packets, hard will drop packets.
     * set to 0 to ignore the particular value
     */
    void setRtpVideoBandwidth(int softLimit, int hardLimit);

private:
  static const int STATS_INTERVAL = 5000;
	SdpInfo remoteSdp_;
	SdpInfo localSdp_;

  Stats thisStats_;

	WebRTCEvent globalState_;

    int bundle_, sequenceNumberFIR_;
    boost::mutex receiveVideoMutex_, updateStateMutex_;
	boost::thread send_Thread_;
	std::queue<dataPacket> sendQueue_;
	WebRtcConnectionEventListener* connEventListener_;
  WebRtcConnectionStatsListener* statsListener_;
	Transport *videoTransport_, *audioTransport_;

    bool sending_;
	void sendLoop();
	void writeSsrc(char* buf, int len, unsigned int ssrc);
  void processRtcpHeaders(char* buf, int len, unsigned int ssrc);
	int deliverAudioData_(char* buf, int len);
	int deliverVideoData_(char* buf, int len);
  int deliverFeedback_(char* buf, int len);
    // directly queue feedback to proper transport
    int queueFeedback(char* buf, int len);

  // changes the outgoing payload type for in the given data packet
  void changeDeliverPayloadType(dataPacket *dp, packetType type);
  // parses incoming payload type, replaces occurence in buf
  void parseIncomingPayloadType(char *buf, int len, packetType type);

  
	bool audioEnabled_;
	bool videoEnabled_;

	int stunPort_, minPort_, maxPort_;
	std::string stunServer_;

	boost::condition_variable cond_;
    webrtc::FecReceiverImpl fec_receiver_;

    // soft and hard limit for video data on this connection
    int rtpLimitSoft_, rtpLimitHard_;

    struct RtcpData {
        // lost packets - list and length
        uint32_t *nackList;
        int nackLen;

        // current values - tracks packet lost for fraction calculation
        int packetCount, lostPacketCount;

        uint32_t ssrc;
        uint32_t totalPacketsLost;
        uint32_t sequenceCycles:16;
        uint32_t sequenceNumber:16;
        // last SR field
        uint32_t lastSrTimestamp;
        // required to properly calculate DLSR
        struct timeval lastSrReception;

        // to prevent sending too many reports, track time of last
        struct timeval lastRrSent;
        // flag to send receiver report
        bool requestRr;
        bool hasSentFirstRr;

        // time based data flow limits
        float allowedSize, desiredSize;
        struct timeval timestamp;
        // should send pli?
        bool shouldSendPli;
        struct timeval lastPliSent;

        // lock for any blocking data change
        boost::mutex dataLock;
    } rtcpData_;
    // simple log output of a buffer
    void logBuffer(char *buf, int len);

    void discardPacket(char *buf, int len);
    void checkPacket(char **p_buf, int *p_len);

    // attach a packet number to those nack'd
    void addNackPacket(uint16_t seqNum, struct RtcpData *pData);
    // analyze feedback - to integrate information in what should be sent to publisher
    void analyzeFeedback(char *buf, int len, struct RtcpData *pData);
    // check transport state of this packet, checks for nack listing, etc
    bool checkTransport(char *buf, int len, struct RtcpData *pData);
    // send a receiver report
    void sendReceiverReport(struct RtcpData *pData);
};

} /* namespace erizo */
#endif /* WEBRTCCONNECTION_H_ */
