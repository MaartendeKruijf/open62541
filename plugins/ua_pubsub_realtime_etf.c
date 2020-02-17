/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.

 * Copyright (c) 2019-2020 Kalycito Infotech Private Limited
 */

/* This file contains socket address handling and tx time calculation for real time publish */

#include <open62541/plugin/network.h>
#include <open62541/plugin/log_stdout.h>
#include <include/open62541/plugin/pubsub_realtime_etf.h>
#ifdef UA_ENABLE_PUBSUB_ETH_UADP
#include <linux/if_packet.h>
#endif
#include "poll.h"
#include "linux/errqueue.h"
#include "time.h"

#define   MICRO_SECONDS                                   1000
#define   MILLI_SECONDS                                   1000 * 1000
#define   SECONDS                                         1000 * 1000 * 1000
#define   CYCLE_TIME                                      250 * 1000
#define   QBV_OFFSET                                      25
#define   SECONDS_INCREMENT                               1
#define   FAILURE_EXIT                                    -1
#define   SHIFT_32BITS                                    32

struct timespec        nextCycleStartTime;
UA_Boolean             firstPacket     = UA_TRUE;
ssize_t                dataCount;
UA_Int32               errorCount;
UA_UInt64              txtime;
/* Qbv offset is 5us for i5. For Mbox, qbv offset is 25us */
UA_Int64               qbv_offset      = (QBV_OFFSET * MICRO_SECONDS);
static UA_Int32        txTimeEnable    = 1;
static unsigned char   txBuffer[256];

static ssize_t sendWithTxTime(UA_PubSubChannel *channel, void *buffer, UA_Int32 length, UA_UInt64 transmission_time) {
    /* Send the data packet with the tx time */
    char dataPacket[CMSG_SPACE(sizeof(transmission_time))] = {0};
    /* Structure for messages sent and received */
    struct msghdr         message;
    /* Structure for scattering or gathering of input/output */
    struct iovec          inputOutputVec;
    ssize_t               msgCount;

#if defined(UA_ENABLE_PUBSUB_ETH_UADP)
    UA_PubSubChannelDataEthernet *channelDataEthernet =
        (UA_PubSubChannelDataEthernet *) channel->handle;

    /* Structure for socket internet address */
    struct sockaddr_ll socketAddress = { 0 };

    socketAddress.sll_family   = AF_PACKET;
    socketAddress.sll_ifindex  = channelDataEthernet->ifindex;
    socketAddress.sll_protocol = htons(ETHERTYPE_UADP);

    inputOutputVec.iov_base   = buffer;
    inputOutputVec.iov_len    = (size_t)length;

    memset(&message, 0, sizeof(message));
    /* Provide message name / optional address */
    message.msg_name          = &socketAddress;
    /* Provide message address size in bytes */
    message.msg_namelen       = sizeof(socketAddress);
    /* Provide array of input/output buffers */
    message.msg_iov           = &inputOutputVec;
    /* Provide the number of elements in the array */
    message.msg_iovlen        = 1;
#else
    UA_PubSubChannelDataUDPMC *channelDatagram = (UA_PubSubChannelDataUDPMC *) channel->handle;
    /* Structure for socket internet address */
    struct sockaddr_in socketAddress;
    memcpy(&socketAddress, channelDatagram->ai_addr, sizeof(struct sockaddr_in));
    /* Provide the base address and length */
    inputOutputVec.iov_base   = buffer;
    inputOutputVec.iov_len    = (size_t)length;

    memset(&message, 0, sizeof(message));
    /* Provide message name / optional address */
    message.msg_name          = &socketAddress;
    /* Provide message address size in bytes */
    message.msg_namelen       = sizeof(socketAddress);
    /* Provide array of input/output buffers */
    message.msg_iov           = &inputOutputVec;
    /* Provide the number of elements in the array */
    message.msg_iovlen        = 1;
#endif
    /*
     * We specify the transmission time in the CMSG.
     */
    if (txTimeEnable) {
        /* Provide the necessary data */
        message.msg_control    = dataPacket;
        /* Provide the size of necessary bytes */
        message.msg_controllen = sizeof(dataPacket);
        /* Structure for storing the necessary data */
        struct cmsghdr*       controlMsg;
        /* Control message created for tx time */
        controlMsg                         = CMSG_FIRSTHDR(&message);
        controlMsg->cmsg_level             = SOL_SOCKET;
        controlMsg->cmsg_type              = SCM_TXTIME;
        controlMsg->cmsg_len               = CMSG_LEN(sizeof(__u64));
        *((__u64 *) CMSG_DATA(controlMsg)) = transmission_time;
    }

    msgCount = sendmsg(channel->sockfd, &message, 0);
    if (msgCount < 1) {
        printf("sendmessage failed");
        return msgCount;
    }

    return msgCount;
}

static int sockErrorQueueProcess(int fd) {
    /* Function to handle error message queues */
    uint8_t dataErrorPacket[CMSG_SPACE(sizeof(struct sock_extended_err))];
    unsigned char errorBuffer[sizeof(txBuffer)];
    /* Structure for storing the error data */
    struct cmsghdr* controlErrorMsg;

    /* Structure for gathering of input/output error buffers */
    struct iovec inputOutputErrorVec = {
        .iov_base = errorBuffer,
        .iov_len  = sizeof(errorBuffer)
    };

    /* Structure for error messages sent and received */
    struct msghdr messageError = {
         .msg_iov        = &inputOutputErrorVec,
         .msg_iovlen     = 1,
         .msg_control    = dataErrorPacket,
         .msg_controllen = sizeof(dataErrorPacket)
    };

    if (recvmsg(fd, &messageError, MSG_ERRQUEUE) == FAILURE_EXIT) {
        UA_LOG_ERROR(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "recvmsg failed");
        return FAILURE_EXIT;
    }

    controlErrorMsg = CMSG_FIRSTHDR(&messageError);
    while (controlErrorMsg != NULL) {
        /* Structure for storing the error queue*/
        struct sock_extended_err* sockErrorQueue;
        sockErrorQueue = (struct sock_extended_err *) CMSG_DATA(controlErrorMsg);
        if (sockErrorQueue->ee_origin == SOCKET_EE_ORIGIN_TRANSMISSION_TIME) {
            __u64 timeStamp = 0;
            timeStamp = ((__u64) sockErrorQueue->ee_data << SHIFT_32BITS) + sockErrorQueue->ee_info;
            switch (sockErrorQueue->ee_code) {
            case SOCKET_EE_CODE_TRANSMISSION_TIME_INVALID_PARAM:
                UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "packet with timeStamp %llu dropped due to invalid params\n", timeStamp);
                return 0;
            case SOCKET_EE_CODE_TRANSMISSION_TIME_MISSED:
                UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "packet with timeStamp %llu dropped due to missed deadline\n", timeStamp);
                return 0;
                default:
                    return -1;
            }
        }

        controlErrorMsg = CMSG_NXTHDR(&messageError, controlErrorMsg);
    }

    return UA_STATUSCODE_GOOD;
}


static void nanoSecondFieldConversion(struct timespec *timeSpecValue) {
    /* Check if ns field is greater than '1 ns less than 1sec' */
    while (timeSpecValue->tv_nsec > (SECONDS -1)) {
        /* Move to next second and remove it from ns field */
        timeSpecValue->tv_sec  += SECONDS_INCREMENT;
        timeSpecValue->tv_nsec -= SECONDS;
    }

}

/* txtime calculation for UDP */
UA_StatusCode
txtimecalc_udp(UA_PubSubChannel *channel, UA_ExtensionObject *transportSettings, const UA_ByteString *buf) {
    /* Function for txtime calculation */
    UA_Int32 errorQueueCheck;
    struct   pollfd p_fd = {
        .fd = channel->sockfd,
    };

    if (firstPacket == UA_TRUE)
    {
        clock_gettime(CLOCK_TAI, &nextCycleStartTime);
        nextCycleStartTime.tv_nsec = CYCLE_TIME + qbv_offset;
        firstPacket = UA_FALSE;
    }
    else
    {
        nextCycleStartTime.tv_nsec += CYCLE_TIME;
        nanoSecondFieldConversion(&nextCycleStartTime);
    }

    /* Calculate the txtime and use txtime to publish the packet at the configured time */
    txtime = (UA_UInt64)nextCycleStartTime.tv_sec * SECONDS + (UA_UInt64)nextCycleStartTime.tv_nsec;
    if (errorCount == 0) {
        dataCount = sendWithTxTime(channel, buf->data, (UA_Int32)buf->length, txtime);
        if (dataCount != (UA_Int32)(buf->length)) {
            return UA_STATUSCODE_BADINTERNALERROR;
        }

         /* Check if errors are pending on the error queue. */
         errorQueueCheck = poll(&p_fd, 1, 0);
         if (errorQueueCheck == 1 && p_fd.revents & POLLERR) {
             if (!sockErrorQueueProcess(channel->sockfd))
                 return UA_STATUSCODE_BADINTERNALERROR; /* TODO Modified the return value. Need to change this */
         }
    }

    return UA_STATUSCODE_GOOD;
}

/* txtime calculation for Ethernet */
UA_StatusCode
txtimecalc_ethernet(UA_PubSubChannel *channel, UA_ExtensionObject *transportSettigns, void *bufSend, int lenBuf) {
    UA_Int32 errorQueueCheck;
    struct   pollfd p_fd = {
        .fd = channel->sockfd,
    };

    if (firstPacket == UA_TRUE)
    {
        clock_gettime(CLOCK_TAI, &nextCycleStartTime);
        nextCycleStartTime.tv_nsec = CYCLE_TIME + qbv_offset;
        firstPacket = UA_FALSE;
    }
    else
    {
        nextCycleStartTime.tv_nsec += CYCLE_TIME;
        nanoSecondFieldConversion(&nextCycleStartTime);
    }

    /* Calculate the txtime and use txtime to publish the packet at the configured time */
    txtime = (long long unsigned int)nextCycleStartTime.tv_sec * SECONDS + (long long unsigned int)nextCycleStartTime.tv_nsec;

    if (errorCount == 0) {
        dataCount = sendWithTxTime(channel, bufSend, (int)lenBuf, txtime);
        if (dataCount != (UA_Int32)lenBuf) {
            return UA_STATUSCODE_BADINTERNALERROR;
        }

    /* Check if errors are pending on the error queue. */
    errorQueueCheck = poll(&p_fd, 1, 0);
    if (errorQueueCheck == 1 && p_fd.revents & POLLERR) {
        if (!sockErrorQueueProcess(channel->sockfd))
            return UA_STATUSCODE_BADINTERNALERROR; /* TODO Modified the return value. Need to change this */
        }
    }

    return UA_STATUSCODE_GOOD;
}
