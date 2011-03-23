/*  =========================================================================
    zloop - event-driven reactor

    -------------------------------------------------------------------------
    Copyright (c) 1991-2011 iMatix Corporation <www.imatix.com>
    Copyright other contributors as noted in the AUTHORS file.

    This file is part of zapi, the C binding for 0MQ: http://zapi.zeromq.org.

    This is free software; you can redistribute it and/or modify it under the
    terms of the GNU Lesser General Public License as published by the Free
    Software Foundation; either version 3 of the License, or (at your option)
    any later version.

    This software is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABIL-
    ITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
    Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
    =========================================================================
*/

#include "../include/zapi_prelude.h"
#include "../include/zctx.h"
#include "../include/zlist.h"
#include "../include/zstr.h"
#include "../include/zloop.h"

//  Structure of our class

struct _zloop_t {
    zlist_t *readers;           //  List of sockets for reading
    zlist_t *timers;            //  List of timers
    zmq_pollitem_t *pollset;    //  zmq_poll set
    Bool dirty;                 //  True if pollset needs rebuilding
};

//  Readers and timers are held as small structures of their own
typedef struct {
    void *socket;
    zloop_fn *handler;
    void *args;
} s_reader_t;
    
typedef struct {
    size_t delay;
    size_t times;
    zloop_fn *handler;
    void *args;
    uint64_t when;              //  Clock time when alarm goes off
} s_timer_t;

static s_reader_t *
s_reader_new (void *socket, zloop_fn handler, void *args)
{
    s_reader_t *reader = (s_reader_t *) zmalloc (sizeof (s_reader_t));
    reader->socket = socket;
    reader->handler = handler;
    reader->args = args;
    return reader;
}

static s_timer_t *
s_timer_new (size_t delay, size_t times, zloop_fn handler, void *args) 
{
    s_timer_t *timer = (s_timer_t *) zmalloc (sizeof (s_timer_t));
    timer->delay = delay;
    timer->times = times;
    timer->handler = handler;
    timer->args = args;
    return timer;
}


//  --------------------------------------------------------------------------
//  Constructor

zloop_t *
zloop_new (void)
{
    zloop_t
        *self;

    self = (zloop_t *) zmalloc (sizeof (zloop_t));
    self->readers = zlist_new ();
    self->timers = zlist_new ();
    return self;
}


//  --------------------------------------------------------------------------
//  Destructor

void
zloop_destroy (zloop_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        zloop_t *self = *self_p;

        //  Destroy list of readers
        while (zlist_size (self->readers))
            free (zlist_pop (self->readers));
        zlist_destroy (&self->readers);
        
        //  Destroy list of timers
        while (zlist_size (self->timers))
            free (zlist_pop (self->timers));
        zlist_destroy (&self->timers);
        
        free (self->pollset);
        free (self);
        *self_p = NULL;
    }
}


//  --------------------------------------------------------------------------
//  Register a socket with the reactor. When the socket has input, will call 
//  the handler, passing the args. Returns 0 if OK, -1 if there was an error.
//  If you register the socket more than once, each instance will invoke its
//  corresponding handler.

int
zloop_reader (zloop_t *self, void *socket, zloop_fn handler, void *args)
{
    assert (self);
    zlist_push (self->readers, s_reader_new (socket, handler, args));
    self->dirty = TRUE;         //  Rebuild will be needed
    return 0;
}


//  --------------------------------------------------------------------------
//  Cancel a socket from the reactor. If the socket was not previously 
//  registered, does nothing. If the socket was registered more than once,
//  cancels only the first instance.

void
zloop_cancel (zloop_t *self, void *socket)
{
    assert (self);
    s_reader_t *reader = (s_reader_t *) zlist_first (self->readers);
    while (reader) {
        if (reader->socket == socket) {
            zlist_remove (self->readers, reader);
            self->dirty = TRUE;     //  Rebuild will be needed
            break;
        }
        reader = (s_reader_t *) zlist_next (self->readers);
    }
}


//  Return current system clock as milliseconds
static int64_t
s_clock (void)
{
#if (defined (__WINDOWS__))
    SYSTEMTIME st;
    GetSystemTime (&st);
    return (int64_t) st.wSecond * 1000 + st.wMilliseconds;
#else
    struct timeval tv;
    gettimeofday (&tv, NULL);
    return (int64_t) (tv.tv_sec * 1000 + tv.tv_usec / 1000);
#endif
}


//  --------------------------------------------------------------------------
//  Register a timer that expires after some delay and repeats some number of
//  times. At each expiry, will call the handler, passing the args. To
//  run a timer forever, use 0 times. Returns 0 if OK, -1 if there was an 
//  error.

int
zloop_timer (zloop_t *self, size_t delay, size_t times, zloop_fn handler, void *args)
{
    assert (self);
    zlist_push (self->timers, s_timer_new (delay, times, handler, args));
    return 0;
}


//  --------------------------------------------------------------------------
//  Start the reactor. Takes control of the thread and returns when the 0MQ
//  context is terminated or the process is interrupted, or any event handler
//  returns -1. Event handlers may register new sockets and timers, and
//  cancel sockets. Returns 0 if interrupted, -1 if cancelled by a handler.

int
zloop_start (zloop_t *self)
{
    assert (self);
    int rc = 0;

    //  Rebuild pollitem set if necessary
    if (self->dirty) {
        free (self->pollset);
        self->pollset = zmalloc (
            zlist_size (self->readers) * sizeof (zmq_pollitem_t));

        s_reader_t *reader = (s_reader_t *) zlist_first (self->readers);
        uint item_nbr = 0;
        while (reader) {
            self->pollset [item_nbr].socket = reader->socket;
            self->pollset [item_nbr].events = ZMQ_POLLIN;
            reader = (s_reader_t *) zlist_next (self->readers);
            item_nbr++;
        }
    }
    //  Recalculate all timers now
    s_timer_t *timer = (s_timer_t *) zlist_first (self->timers);
    while (timer) {
        timer->when = s_clock () + timer->delay;
        timer = (s_timer_t *) zlist_next (self->timers);
    }

    //  Main reactor loop
    while (!zctx_interrupted) {
        //  Calculate tickless timer, up to 1 hour
        uint64_t tickless = s_clock () + 1000 * 3600;
        s_timer_t *timer = (s_timer_t *) zlist_first (self->timers);
        while (timer) {
            //  Find earliest timer
            if (tickless > timer->when)
                tickless = timer->when;
            timer = (s_timer_t *) zlist_next (self->timers);
        }
        long timeout = (long) (tickless - s_clock ()) * 1000;
        if (timeout < 0)
            timeout = 0;
        rc = zmq_poll (self->pollset, zlist_size (self->readers), timeout);
        if (rc == -1 && errno == ETERM) {
            rc = 0;
            break;              //  Context has been shut down
        }
        //  Handle any timers that have now expired
        timer = (s_timer_t *) zlist_first (self->timers);
        while (timer) {
            if (s_clock () > timer->when) {
                rc = timer->handler (self, NULL, timer->args);
                if (rc == -1)
                    break;      //  Timer handler signalled break
                if (timer->times && --timer->times == 0) {
                    zlist_remove (self->timers, timer);
                    free (timer);
                }
                else
                    timer->when = timer->delay + s_clock ();
            }
            timer = (s_timer_t *) zlist_next (self->timers);
        }
        //  Handle any readers that are ready
        s_reader_t *reader = (s_reader_t *) zlist_first (self->readers);
        uint item_nbr = 0;
        while (reader && rc != -1) {
            if (self->pollset [item_nbr].revents & ZMQ_POLLIN) {
                rc = reader->handler (self, reader->socket, reader->args);
                if (rc == -1)
                    break;      //  Reader handler signalled break
            }
            reader = (s_reader_t *) zlist_next (self->readers);
            item_nbr++;
        }
        if (rc == -1)
            break;
    }
    return rc;
}


//  --------------------------------------------------------------------------
//  Selftest

int s_timer_event (zloop_t *loop, void *socket, void *output)
{
    zstr_send (output, "PING");
    return 0;
}

int s_socket_event (zloop_t *loop, void *socket, void *args)
{
    //  Just end the reactor
    return -1;
}

int
zloop_test (Bool verbose)
{
    printf (" * zloop: ");
    zctx_t *ctx = zctx_new ();
    
    void *output = zctx_socket_new (ctx, ZMQ_PAIR);
    zmq_bind (output, "inproc://zloop.test");
    void *input = zctx_socket_new (ctx, ZMQ_PAIR);
    zmq_connect (input, "inproc://zloop.test");

    zloop_t *loop = zloop_new ();
    assert (loop);
    
    //  After 10 msecs, send a ping message to output
    zloop_timer (loop, 10, 1,  s_timer_event, output);
    //  When we get the ping message, end the reactor
    zloop_reader (loop, input, s_socket_event, NULL);
    zloop_start (loop);
    
    zloop_destroy (&loop);
    assert (loop == NULL);

    zctx_destroy (&ctx);
    printf ("OK\n");
    return 0;
}