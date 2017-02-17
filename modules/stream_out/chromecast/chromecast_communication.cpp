/*****************************************************************************
 * chromecast_communication.cpp: Handle chromecast protocol messages
 *****************************************************************************
 * Copyright © 2014-2017 VideoLAN
 *
 * Authors: Adrien Maglo <magsoft@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Steve Lhomme <robux4@videolabs.io>
 *          Hugo Beauzée-Luyssen <hugo@beauzee.fr>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "chromecast.h"
#ifdef HAVE_POLL
# include <poll.h>
#endif

/* deadline regarding pong we expect after pinging the receiver */
#define PONG_WAIT_TIME 500
#define PONG_WAIT_RETRIES 2

ChromecastCommunication::ChromecastCommunication( vlc_object_t* p_module )
    : p_module( p_module )
    , i_sock_fd( -1 )
    , p_creds( NULL )
    , p_tls( NULL )
    , i_receiver_requestId( 0 )
    , i_requestId( 0 )
{
}

bool ChromecastCommunication::connect( const char* targetIP, unsigned int devicePort )
{
    if (devicePort == 0)
        devicePort = CHROMECAST_CONTROL_PORT;
    i_sock_fd = net_ConnectTCP( p_module, targetIP, devicePort);
    if (i_sock_fd < 0)
        return false;

    char psz_localIP[NI_MAXNUMERICHOST];
    if ( net_GetSockAddress( i_sock_fd, psz_localIP, NULL ) )
    {
        msg_Err( p_module, "Cannot get local IP address" );
        return false;
    }
    serverIp = psz_localIP;

    p_creds = vlc_tls_ClientCreate( p_module->obj.parent );
    if (p_creds == NULL)
    {
        msg_Err( p_module, "Failed to create TLS client" );
        net_Close(i_sock_fd);
        return false;
    }

    p_tls = vlc_tls_ClientSessionCreateFD( p_creds, i_sock_fd, targetIP, "tcps", NULL, NULL );

    if (p_tls == NULL)
    {
        msg_Err( p_module, "Failed to create client session" );
        net_Close(i_sock_fd);
        vlc_tls_Delete(p_creds);
        return false;
    }
    return true;
}

ChromecastCommunication::~ChromecastCommunication()
{
    disconnect();
}

void ChromecastCommunication::disconnect()
{
    if ( p_tls != NULL )
    {
        vlc_tls_Close(p_tls);
        vlc_tls_Delete(p_creds);
        p_tls = NULL;
    }
}

/**
 * @brief Build a CastMessage to send to the Chromecast
 * @param namespace_ the message namespace
 * @param payloadType the payload type (CastMessage_PayloadType_STRING or
 * CastMessage_PayloadType_BINARY
 * @param payload the payload
 * @param destinationId the destination idenifier
 * @return the generated CastMessage
 */
void ChromecastCommunication::buildMessage(const std::string & namespace_,
                              const std::string & payload,
                              const std::string & destinationId,
                              castchannel::CastMessage_PayloadType payloadType)
{
    castchannel::CastMessage msg;

    msg.set_protocol_version(castchannel::CastMessage_ProtocolVersion_CASTV2_1_0);
    msg.set_namespace_(namespace_);
    msg.set_payload_type(payloadType);
    msg.set_source_id("sender-vlc");
    msg.set_destination_id(destinationId);
    if (payloadType == castchannel::CastMessage_PayloadType_STRING)
        msg.set_payload_utf8(payload);
    else // CastMessage_PayloadType_BINARY
        msg.set_payload_binary(payload);

    sendMessage(msg);
}

/**
 * @brief Receive a data packet from the Chromecast
 * @param p_module the module to log with
 * @param b_msgReceived returns true if a message has been entirely received else false
 * @param i_payloadSize returns the payload size of the message received
 * @return the number of bytes received of -1 on error
 */
int ChromecastCommunication::recvPacket(bool *b_msgReceived,
                          uint32_t &i_payloadSize,
                          unsigned *pi_received, uint8_t *p_data, bool *pb_pingTimeout,
                          int *pi_wait_delay, int *pi_wait_retries)
{
    struct pollfd ufd[1];
    ufd[0].fd = i_sock_fd;
    ufd[0].events = POLLIN;

    /* The Chromecast normally sends a PING command every 5 seconds or so.
     * If we do not receive one after 6 seconds, we send a PING.
     * If after this PING, we do not receive a PONG, then we consider the
     * connection as dead. */
    ssize_t val = vlc_poll_i11e(ufd, 1, *pi_wait_delay);
    if ( val == -1 && errno != EINTR )
        return -1;

    if (val == 0)
    {
        if (*pb_pingTimeout)
        {
            if (!*pi_wait_retries)
            {
                msg_Err( p_module, "No PONG answer received from the Chromecast");
                return 0; // Connection died
            }
            (*pi_wait_retries)--;
        }
        else
        {
            /* now expect a pong */
            *pi_wait_delay = PONG_WAIT_TIME;
            *pi_wait_retries = PONG_WAIT_RETRIES;
            msg_Warn( p_module, "No PING received from the Chromecast, sending a PING");
        }
        *pb_pingTimeout = true;
    }
    else
    {
        *pb_pingTimeout = false;
        /* reset to default ping waiting */
        *pi_wait_delay = PING_WAIT_TIME;
        *pi_wait_retries = PING_WAIT_RETRIES;
    }

    int i_ret = 0;
    if ( ufd[0].revents & POLLIN )
    {
        /* we have received stuff */

        /* Packet structure:
         * +------------------------------------+------------------------------+
         * | Payload size (uint32_t big endian) |         Payload data         |
         * +------------------------------------+------------------------------+ */
        while (*pi_received < PACKET_HEADER_LEN)
        {
            // We receive the header.
            i_ret = tls_Recv(p_tls, p_data + *pi_received, PACKET_HEADER_LEN - *pi_received);
            if (i_ret <= 0)
                return i_ret;
            *pi_received += i_ret;
        }

        // We receive the payload.

        // Get the size of the payload
        i_payloadSize = U32_AT( p_data );
        const uint32_t i_maxPayloadSize = PACKET_MAX_LEN - PACKET_HEADER_LEN;

        if (i_payloadSize > i_maxPayloadSize)
        {
            // Error case: the packet sent by the Chromecast is too long: we drop it.
            msg_Err( p_module, "Packet too long: droping its data");

            uint32_t i_size = i_payloadSize - (*pi_received - PACKET_HEADER_LEN);
            if (i_size > i_maxPayloadSize)
                i_size = i_maxPayloadSize;

            i_ret = tls_Recv(p_tls, p_data + PACKET_HEADER_LEN, i_size);
            if (i_ret <= 0)
                return i_ret;
            *pi_received += i_ret;

            if (*pi_received < i_payloadSize + PACKET_HEADER_LEN)
                return i_ret;

            *pi_received = 0;
            return -1;
        }

        // Normal case
        i_ret = tls_Recv(p_tls, p_data + *pi_received,
                         i_payloadSize - (*pi_received - PACKET_HEADER_LEN));
        if (i_ret <= 0)
            return i_ret;
        *pi_received += i_ret;

        if (*pi_received < i_payloadSize + PACKET_HEADER_LEN)
            return i_ret;

        assert(*pi_received == i_payloadSize + PACKET_HEADER_LEN);
        *pi_received = 0;
        *b_msgReceived = true;
    }

    if ( val == -1 && errno == EINTR )
        /* we have stuff to send */
        i_ret = 1;

    return i_ret;
}


/*****************************************************************************
 * Message preparation
 *****************************************************************************/
void ChromecastCommunication::msgAuth()
{
    castchannel::DeviceAuthMessage authMessage;
    authMessage.mutable_challenge();

    buildMessage(NAMESPACE_DEVICEAUTH, authMessage.SerializeAsString(),
                 DEFAULT_CHOMECAST_RECEIVER, castchannel::CastMessage_PayloadType_BINARY);
}


void ChromecastCommunication::msgPing()
{
    std::string s("{\"type\":\"PING\"}");
    buildMessage( NAMESPACE_HEARTBEAT, s, DEFAULT_CHOMECAST_RECEIVER );
}


void ChromecastCommunication::msgPong()
{
    std::string s("{\"type\":\"PONG\"}");
    buildMessage( NAMESPACE_HEARTBEAT, s, DEFAULT_CHOMECAST_RECEIVER );
}

void ChromecastCommunication::msgConnect( const std::string& destinationId )
{
    std::string s("{\"type\":\"CONNECT\"}");
    buildMessage( NAMESPACE_CONNECTION, s, destinationId );
}

void ChromecastCommunication::msgReceiverClose( const std::string& destinationId )
{
    std::string s("{\"type\":\"CLOSE\"}");
    buildMessage( NAMESPACE_CONNECTION, s, destinationId );
}

void ChromecastCommunication::msgReceiverGetStatus()
{
    std::stringstream ss;
    ss << "{\"type\":\"GET_STATUS\","
       <<  "\"requestId\":" << i_receiver_requestId++ << "}";

    buildMessage( NAMESPACE_RECEIVER, ss.str(), DEFAULT_CHOMECAST_RECEIVER );
}

void ChromecastCommunication::msgReceiverLaunchApp()
{
    std::stringstream ss;
    ss << "{\"type\":\"LAUNCH\","
       <<  "\"appId\":\"" << APP_ID << "\","
       <<  "\"requestId\":" << i_receiver_requestId++ << "}";

    buildMessage( NAMESPACE_RECEIVER, ss.str(), DEFAULT_CHOMECAST_RECEIVER );
}

void ChromecastCommunication::msgPlayerGetStatus( const std::string& destinationId )
{
    std::stringstream ss;
    ss << "{\"type\":\"GET_STATUS\","
       <<  "\"requestId\":" << i_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

std::string ChromecastCommunication::GetMedia( unsigned int i_port,
                                               const std::string& title, const std::string& artwork,
                                               const std::string& mime )
{
    std::stringstream ss;

    if ( title.size() )
    {
        ss << "\"metadata\":{"
           << " \"metadataType\":0"
           << ",\"title\":\"" << title << "\"";

        if ( artwork.size() && !strncmp(artwork.c_str(), "http", 4))
            ss << ",\"images\":[\"" << artwork << "\"]";

        ss << "},";
    }

    std::stringstream chromecast_url;
    chromecast_url << "http://" << serverIp << ":" << i_port << "/stream";

    msg_Dbg( p_module, "s_chromecast_url: %s", chromecast_url.str().c_str());

    ss << "\"contentId\":\"" << chromecast_url.str() << "\""
       << ",\"streamType\":\"LIVE\""
       << ",\"contentType\":\"" << mime << "\"";

    return ss.str();
}

void ChromecastCommunication::msgPlayerLoad( const std::string& destinationId, unsigned int i_port,
                                             const std::string& title, const std::string& artwork,
                                             const std::string& mime )
{
    std::stringstream ss;
    ss << "{\"type\":\"LOAD\","
       <<  "\"media\":{" << GetMedia( i_port, title, artwork, mime ) << "},"
       <<  "\"autoplay\":\"false\","
       <<  "\"requestId\":" << i_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

void ChromecastCommunication::msgPlayerPlay( const std::string& destinationId, const std::string& mediaSessionId )
{
    assert(!mediaSessionId.empty());

    std::stringstream ss;
    ss << "{\"type\":\"PLAY\","
       <<  "\"mediaSessionId\":" << mediaSessionId << ","
       <<  "\"requestId\":" << i_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

void ChromecastCommunication::msgPlayerStop( const std::string& destinationId, const std::string& mediaSessionId )
{
    assert(!mediaSessionId.empty());

    std::stringstream ss;
    ss << "{\"type\":\"STOP\","
       <<  "\"mediaSessionId\":" << mediaSessionId << ","
       <<  "\"requestId\":" << i_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

void ChromecastCommunication::msgPlayerPause( const std::string& destinationId, const std::string& mediaSessionId )
{
    assert(!mediaSessionId.empty());

    std::stringstream ss;
    ss << "{\"type\":\"PAUSE\","
       <<  "\"mediaSessionId\":" << mediaSessionId << ","
       <<  "\"requestId\":" << i_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

void ChromecastCommunication::msgPlayerSetVolume( const std::string& destinationId, const std::string& mediaSessionId, float f_volume, bool b_mute )
{
    assert(!mediaSessionId.empty());

    if ( f_volume < 0.0 || f_volume > 1.0)
        return;

    std::stringstream ss;
    ss << "{\"type\":\"SET_VOLUME\","
       <<  "\"volume\":{\"level\":" << f_volume << ",\"muted\":" << ( b_mute ? "true" : "false" ) << "},"
       <<  "\"mediaSessionId\":" << mediaSessionId << ","
       <<  "\"requestId\":" << i_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

void ChromecastCommunication::msgPlayerSeek( const std::string& destinationId, const std::string& mediaSessionId, const std::string& currentTime )
{
    assert(!mediaSessionId.empty());

    std::stringstream ss;
    ss << "{\"type\":\"SEEK\","
       <<  "\"currentTime\":" << currentTime << ","
       <<  "\"mediaSessionId\":" << mediaSessionId << ","
       <<  "\"requestId\":" << i_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

/**
 * @brief Send a message to the Chromecast
 * @param msg the CastMessage to send
 * @return vlc error code
 */
int ChromecastCommunication::sendMessage( const castchannel::CastMessage &msg )
{
    int i_size = msg.ByteSize();
    uint8_t *p_data = new(std::nothrow) uint8_t[PACKET_HEADER_LEN + i_size];
    if (p_data == NULL)
        return VLC_ENOMEM;

#ifndef NDEBUG
    msg_Dbg( p_module, "sendMessage: %s->%s %s", msg.namespace_().c_str(), msg.destination_id().c_str(), msg.payload_utf8().c_str());
#endif

    SetDWBE(p_data, i_size);
    msg.SerializeWithCachedSizesToArray(p_data + PACKET_HEADER_LEN);

    int i_ret = tls_Send(p_tls, p_data, PACKET_HEADER_LEN + i_size);
    delete[] p_data;
    if (i_ret == PACKET_HEADER_LEN + i_size)
        return VLC_SUCCESS;

    msg_Warn( p_module, "failed to send message %s (%s)", msg.payload_utf8().c_str(), strerror( errno ) );

    return VLC_EGENERIC;
}

void ChromecastCommunication::pushMediaPlayerMessage( const std::string& destinationId, const std::stringstream & payload )
{
    assert(!destinationId.empty());
    buildMessage( NAMESPACE_MEDIA, payload.str(), destinationId );
}