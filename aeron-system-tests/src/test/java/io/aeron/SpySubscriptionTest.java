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
package io.aeron;

import io.aeron.driver.MediaDriver;
import io.aeron.logbuffer.FragmentHandler;
import io.aeron.logbuffer.Header;
import org.agrona.DirectBuffer;
import org.agrona.concurrent.UnsafeBuffer;
import org.junit.Test;
import org.junit.experimental.theories.DataPoint;
import org.junit.experimental.theories.Theories;
import org.junit.experimental.theories.Theory;
import org.junit.runner.RunWith;

import static io.aeron.SystemTestHelper.spyForChannel;
import static org.mockito.Mockito.*;

@RunWith(Theories.class)
public class SpySubscriptionTest
{
    @DataPoint
    public static final String UNICAST_CHANNEL = "aeron:udp?endpoint=localhost:54325";

    @DataPoint
    public static final String MULTICAST_CHANNEL = "aeron:udp?endpoint=224.20.30.39:54326|interface=localhost";

    public static final int STREAM_ID = 1;
    public static final int FRAGMENT_COUNT_LIMIT = 10;
    public static final int PAYLOAD_LENGTH = 10;

    private final FragmentHandler mockFragmentHandler = mock(FragmentHandler.class);
    private final FragmentHandler mockSpyFragmentHandler = mock(FragmentHandler.class);

    @Theory
    @Test(timeout = 10000)
    public void shouldReceivePublishedMessage(final String channel)
    {
        final MediaDriver.Context ctx = new MediaDriver.Context()
            .errorHandler(Throwable::printStackTrace);

        try (MediaDriver ignore = MediaDriver.launch(ctx);
            Aeron aeron = Aeron.connect();
            Publication publication = aeron.addPublication(channel, STREAM_ID);
            Subscription subscription = aeron.addSubscription(channel, STREAM_ID);
            Subscription spy = aeron.addSubscription(spyForChannel(channel), STREAM_ID))
        {
            final UnsafeBuffer srcBuffer = new UnsafeBuffer(new byte[PAYLOAD_LENGTH * 4]);

            for (int i = 0; i < 4; i++)
            {
                srcBuffer.setMemory(i * PAYLOAD_LENGTH, PAYLOAD_LENGTH, (byte)(65 + i));
            }

            for (int i = 0; i < 4; i++)
            {
                while (publication.offer(srcBuffer, i * PAYLOAD_LENGTH, PAYLOAD_LENGTH) < 0L)
                {
                    Thread.yield();
                }
            }

            int numFragments = 0;
            int numSpyFragments = 0;
            do
            {
                numFragments += subscription.poll(mockFragmentHandler, FRAGMENT_COUNT_LIMIT);
                numSpyFragments += spy.poll(mockSpyFragmentHandler, FRAGMENT_COUNT_LIMIT);
            }
            while (numSpyFragments < 4 || numFragments < 4);

            verify(mockFragmentHandler, times(4)).onFragment(
                any(DirectBuffer.class), anyInt(), eq(PAYLOAD_LENGTH), any(Header.class));
            verify(mockSpyFragmentHandler, times(4)).onFragment(
                any(DirectBuffer.class), anyInt(), eq(PAYLOAD_LENGTH), any(Header.class));
        }
        finally
        {
            ctx.deleteAeronDirectory();
        }
    }
}
