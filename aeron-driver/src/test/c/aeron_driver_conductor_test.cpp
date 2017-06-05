/*
 * Copyright 2014-2017 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <array>
#include <cstdint>
#include <thread>
#include <exception>

#include <gtest/gtest.h>

extern "C"
{
#include "aeron_driver_conductor.h"
}

#include "concurrent/ringbuffer/ManyToOneRingBuffer.h"
#include "concurrent/broadcast/CopyBroadcastReceiver.h"

using namespace aeron::concurrent::broadcast;
using namespace aeron::concurrent::ringbuffer;
using namespace aeron::concurrent;
using namespace aeron;

#define STREAM_ID_1 (101)

static int64_t ms_timestamp = 0;

static int64_t test_nano_clock()
{
    return ms_timestamp * 1000 * 1000;
}

static int64_t test_epoch_clock()
{
    return ms_timestamp;
}

static int test_malloc_map_raw_log(
    aeron_mapped_raw_log_t *log, const char *path, bool use_sparse_file, uint64_t term_length)
{
    uint64_t log_length = AERON_LOGBUFFER_COMPUTE_LOG_LENGTH(term_length);

    log->num_mapped_files = 0;
    log->mapped_files[0].length = 0;
    log->mapped_files[0].addr = malloc(log_length);

    for (size_t i = 0; i < AERON_LOGBUFFER_PARTITION_COUNT; i++)
    {
        log->term_buffers[i].addr =
            (uint8_t *)log->mapped_files[0].addr + (i * term_length);
        log->term_buffers[i].length = term_length;
    }

    log->log_meta_data.addr =
        (uint8_t *)log->mapped_files[0].addr + (log_length - AERON_LOGBUFFER_META_DATA_LENGTH);
    log->log_meta_data.length = AERON_LOGBUFFER_META_DATA_LENGTH;

    log->term_length = term_length;
    return 0;
}

static int test_malloc_map_raw_log_close(aeron_mapped_raw_log_t *log)
{
    free(log->mapped_files[0].addr);
    return 0;
}

static uint64_t test_uint64_max_usable_fs_space(const char *path)
{
    return UINT64_MAX;
}

struct TestDriverContext
{
    TestDriverContext()
    {
        ms_timestamp = 0; /* single threaded */

        if (aeron_driver_context_init(&m_context) < 0)
        {
            throw std::runtime_error("could not init context");
        }

        m_context->cnc_map.length = aeron_cnc_length(m_context);
        m_cnc = std::unique_ptr<uint8_t[]>(new uint8_t[m_context->cnc_map.length]);
        m_context->cnc_map.addr = m_cnc.get();

        memset(m_context->cnc_map.addr, 0, m_context->cnc_map.length);

        aeron_driver_fill_cnc_metadata(m_context);

        /* control time */
        m_context->nano_clock = test_nano_clock;
        m_context->epoch_clock = test_epoch_clock;

        /* control files */
        m_context->usable_fs_space_func = test_uint64_max_usable_fs_space;
        m_context->map_raw_log_func = test_malloc_map_raw_log;
        m_context->map_raw_log_close_func = test_malloc_map_raw_log_close;
    }

    virtual ~TestDriverContext()
    {
        m_context->cnc_map.addr = NULL;
        aeron_driver_context_close(m_context);
    }

    aeron_driver_context_t *m_context = NULL;
    std::unique_ptr<uint8_t[]> m_cnc;
};

struct TestDriverConductor
{
    TestDriverConductor(TestDriverContext &context)
    {
        if (aeron_driver_conductor_init(&m_conductor, context.m_context) < 0)
        {
            throw std::runtime_error("could not init context");
        }
    }

    virtual ~TestDriverConductor()
    {
        aeron_driver_conductor_on_close(&m_conductor);
    }

    aeron_driver_conductor_t m_conductor;
};

class DriverConductorTest : public testing::Test
{
public:

    DriverConductorTest() :
        m_command(m_command_buffer, sizeof(m_command_buffer)),
        m_conductor(m_context),
        m_to_clients_buffer(
            m_context.m_context->to_clients_buffer,
            static_cast<util::index_t>(m_context.m_context->to_clients_buffer_length)),
        m_to_clients_receiver(m_to_clients_buffer),
        m_to_clients_copy_receiver(m_to_clients_receiver),
        m_to_driver_buffer(
            m_context.m_context->to_driver_buffer,
            static_cast<util::index_t >(m_context.m_context->to_driver_buffer_length)),
        m_to_driver(m_to_driver_buffer)
    {
    }

    size_t readAllBroadcastsFromConductor(const handler_t& func)
    {
        return (size_t)m_to_clients_copy_receiver.receive(func);
    }

    int64_t nextCorrelationId()
    {
        return m_to_driver.nextCorrelationId();
    }

    inline int writeCommand(int32_t msg_type_id, util::index_t length)
    {
        return m_to_driver.write(msg_type_id, m_command, 0, length) ? 0 : -1;
    }

    int addIpcPublication(int64_t client_id, int64_t correlation_id, int32_t stream_id, bool is_exclusive)
    {
        int32_t msg_type_id = is_exclusive ? AERON_COMMAND_ADD_EXCLUSIVE_PUBLICATION : AERON_COMMAND_ADD_PUBLICATION;
        aeron_publication_command_t &command = m_command.overlayStruct<aeron_publication_command_t>(0);
        command.correlated.client_id = client_id;
        command.correlated.correlation_id = correlation_id;
        command.stream_id = stream_id;
        command.channel_length = sizeof(AERON_IPC_CHANNEL);
        memcpy(command.channel_data, AERON_IPC_CHANNEL, sizeof(AERON_IPC_CHANNEL));

        return writeCommand(msg_type_id, sizeof(aeron_publication_command_t) + sizeof(AERON_IPC_CHANNEL) - 1);
    }

    int removePublication(int64_t client_id, int64_t correlation_id, int64_t registration_id)
    {
        aeron_remove_command_t &command = m_command.overlayStruct<aeron_remove_command_t>(0);
        command.correlated.client_id = client_id;
        command.correlated.correlation_id = correlation_id;
        command.registration_id = registration_id;

        return writeCommand(AERON_COMMAND_REMOVE_PUBLICATION, sizeof(aeron_remove_command_t));
    }

    int addIpcSubscription(int64_t client_id, int64_t correlation_id, int32_t stream_id, int64_t registration_id)
    {
        aeron_subscription_command_t &command = m_command.overlayStruct<aeron_subscription_command_t>(0);
        command.correlated.client_id = client_id;
        command.correlated.correlation_id = correlation_id;
        command.stream_id = stream_id;
        command.registration_correlation_id = registration_id;
        command.channel_length = sizeof(AERON_IPC_CHANNEL);
        memcpy(command.channel_data, AERON_IPC_CHANNEL, sizeof(AERON_IPC_CHANNEL));

        return writeCommand(
            AERON_COMMAND_ADD_SUBSCRIPTION, sizeof(aeron_subscription_command_t) + sizeof(AERON_IPC_CHANNEL) - 1);
    }

    int removeSubscription(int64_t client_id, int64_t correlation_id, int64_t registration_id)
    {
        aeron_remove_command_t &command = m_command.overlayStruct<aeron_remove_command_t>(0);
        command.correlated.client_id = client_id;
        command.correlated.correlation_id = correlation_id;
        command.registration_id = registration_id;

        return writeCommand(AERON_COMMAND_REMOVE_SUBSCRIPTION, sizeof(aeron_remove_command_t));
    }

    aeron_ipc_publication_t *findIpcPublication(int64_t registration_id)
    {
        aeron_driver_conductor_t *conductor = &m_conductor.m_conductor;

        for (size_t i = 0, length = conductor->ipc_publications.length; i < length; i++)
        {
            aeron_ipc_publication_t *publication = conductor->ipc_publications.array[i].publication;

            if (registration_id == publication->conductor_fields.managed_resource.registration_id)
            {
                return publication;
            }
        }

        return nullptr;
    }

    int doWork()
    {
        return aeron_driver_conductor_do_work(&m_conductor.m_conductor);
    }

protected:
    uint8_t m_command_buffer[AERON_MAX_PATH];
    AtomicBuffer m_command;
    TestDriverContext m_context;
    TestDriverConductor m_conductor;

    AtomicBuffer m_to_clients_buffer;
    BroadcastReceiver m_to_clients_receiver;
    CopyBroadcastReceiver m_to_clients_copy_receiver;

    AtomicBuffer m_to_driver_buffer;
    ManyToOneRingBuffer m_to_driver;
};

TEST_F(DriverConductorTest, shouldBeAbleToAddSingleIpcPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);

    doWork();

    ASSERT_NE(findIpcPublication(pub_id), nullptr);

    auto handler = [&](std::int32_t msgTypeId, AtomicBuffer& buffer, util::index_t offset, util::index_t length)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

            aeron_publication_buffers_ready_t &response =
                buffer.overlayStruct<aeron_publication_buffers_ready_t>(offset);

            EXPECT_EQ(response.stream_id, STREAM_ID_1);
            EXPECT_EQ(response.correlation_id, pub_id);
        };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}

