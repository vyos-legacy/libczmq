/*  =========================================================================
    zctx - working with 0MQ contexts

    -------------------------------------------------------------------------
    Copyright (c) 1991-2013 iMatix Corporation <www.imatix.com>
    Copyright other contributors as noted in the AUTHORS file.

    This file is part of CZMQ, the high-level C binding for 0MQ:
    http://czmq.zeromq.org.

    This is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by the 
    Free Software Foundation; either version 3 of the License, or (at your 
    option) any later version.

    This software is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABIL-
    ITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General 
    Public License for more details.

    You should have received a copy of the GNU Lesser General Public License 
    along with this program. If not, see <http://www.gnu.org/licenses/>.
    =========================================================================
*/

/*
@header
    The zctx class wraps 0MQ contexts. It manages open sockets in the context
    and automatically closes these before terminating the context. It provides
    a simple way to set the linger timeout on sockets, and configure contexts
    for number of I/O threads. Sets-up signal (interrupt) handling for the
    process.

    The zctx class has these main features:

    * Tracks all open sockets and automatically closes them before calling
    zmq_term(). This avoids an infinite wait on open sockets.

    * Automatically configures sockets with a ZMQ_LINGER timeout you can
    define, and which defaults to zero. The default behavior of zctx is
    therefore like 0MQ/2.0, immediate termination with loss of any pending
    messages. You can set any linger timeout you like by calling the
    zctx_set_linger() method.

    * Moves the iothreads configuration to a separate method, so that default
    usage is 1 I/O thread. Lets you configure this value.

    * Sets up signal (SIGINT and SIGTERM) handling so that blocking calls
    such as zmq_recv() and zmq_poll() will return when the user presses
    Ctrl-C.

@discuss
@end
*/

#include "../include/czmq.h"

//  Structure of our class

struct _zctx_t {
    void *context;              //  Our 0MQ context
    zlist_t *sockets;           //  Sockets held by this thread
    bool main;                  //  True if we're the main thread
    int iothreads;              //  Number of IO threads, default 1
    int linger;                 //  Linger timeout, default 0
};


//  ---------------------------------------------------------------------
//  Signal handling
//  This is a global variable accessible to CZMQ application code

volatile int zctx_interrupted = 0;

static void
s_signal_handler (int signal_value)
{
    zctx_interrupted = 1;
}


//  --------------------------------------------------------------------------
//  Constructor

zctx_t *
zctx_new (void)
{
    zctx_t
        *self;

    self = (zctx_t *) zmalloc (sizeof (zctx_t));
    if (!self)
        return NULL;

    self->sockets = zlist_new ();
    if (!self->sockets) {
        free (self);
        return NULL;
    }
    self->iothreads = 1;
    self->main = true;
    zsys_handler_set (s_signal_handler);
    return self;
}


//  --------------------------------------------------------------------------
//  Destructor

void
zctx_destroy (zctx_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        zctx_t *self = *self_p;
        while (zlist_size (self->sockets))
            zctx__socket_destroy (self, zlist_first (self->sockets));
        zlist_destroy (&self->sockets);
        if (self->main && self->context)
            zmq_term (self->context);
        free (self);
        *self_p = NULL;
    }
}


//  --------------------------------------------------------------------------
//  Create new shadow context, returns context object. Returns NULL if there
//  wasn't sufficient memory available.

zctx_t *
zctx_shadow (zctx_t *ctx)
{
    zctx_t
        *self;

    //  Shares same 0MQ context but has its own list of sockets so that
    //  we create, use, and destroy sockets only within a single thread.
    self = (zctx_t *) zmalloc (sizeof (zctx_t));
    if (!self)
        return NULL;

    self->context = ctx->context;
    self->sockets = zlist_new ();
    if (!self->sockets) {
        free (self);
        return NULL;
    }
    return self;
}


//  --------------------------------------------------------------------------
//  Configure number of I/O threads in context, only has effect if called
//  before creating first socket. Default I/O threads is 1, sufficient for
//  all except very high volume applications.

void
zctx_set_iothreads (zctx_t *self, int iothreads)
{
    assert (self);
    self->iothreads = iothreads;
}


//  --------------------------------------------------------------------------
//  Configure linger timeout in msecs. Call this before destroying sockets or
//  context. Default is no linger, i.e. any pending messages or connects will
//  be lost.

void
zctx_set_linger (zctx_t *self, int linger)
{
    assert (self);
    self->linger = linger;
}


//  --------------------------------------------------------------------------
//  Deprecated method, does nothing - to be removed after 2013/05/14

void
zctx_set_hwm (zctx_t *self, int hwm)
{
    assert (self);
}

//  --------------------------------------------------------------------------
//  Deprecated method, does nothing - to be removed after 2013/05/14

int
zctx_hwm (zctx_t *self)
{
    assert (self);
    return 0;
}

//  --------------------------------------------------------------------------
//  Return low-level 0MQ context object

void *
zctx_underlying (zctx_t *self)
{
    assert (self);
    return self->context;
}


//  --------------------------------------------------------------------------
//  Create socket within this context, for CZMQ use only

void *
zctx__socket_new (zctx_t *self, int type)
{
    //  Initialize context now if necessary
    assert (self);
    if (!self->context)
        self->context = zmq_init (self->iothreads);
    if (!self->context)
        return NULL;

    //  Create and register socket
    void *zocket = zmq_socket (self->context, type);
    if (!zocket)
        return NULL;

    if (zlist_push (self->sockets, zocket)) {
        zmq_close (zocket);
        return NULL;
    }
    return zocket;
}


//  --------------------------------------------------------------------------
//  Destroy socket within this context, for CZMQ use only

void
zctx__socket_destroy (zctx_t *self, void *zocket)
{
    assert (self);
    assert (zocket);
    zsocket_set_linger (zocket, self->linger);
    zmq_close (zocket);
    zlist_remove (self->sockets, zocket);
}


//  --------------------------------------------------------------------------
//  Selftest

int
zctx_test (bool verbose)
{
    printf (" * zctx: ");

    //  @selftest
    //  Create and destroy a context without using it
    zctx_t *ctx = zctx_new ();
    assert (ctx);
    zctx_destroy (&ctx);
    assert (ctx == NULL);

    //  Create a context with many busy sockets, destroy it
    ctx = zctx_new ();
    assert (ctx);
    zctx_set_iothreads (ctx, 1);
    zctx_set_linger (ctx, 5);       //  5 msecs
    void *s1 = zctx__socket_new (ctx, ZMQ_PAIR);
    void *s2 = zctx__socket_new (ctx, ZMQ_XREQ);
    void *s3 = zctx__socket_new (ctx, ZMQ_REQ);
    void *s4 = zctx__socket_new (ctx, ZMQ_REP);
    void *s5 = zctx__socket_new (ctx, ZMQ_PUB);
    void *s6 = zctx__socket_new (ctx, ZMQ_SUB);
    zsocket_connect (s1, "tcp://127.0.0.1:5555");
    zsocket_connect (s2, "tcp://127.0.0.1:5555");
    zsocket_connect (s3, "tcp://127.0.0.1:5555");
    zsocket_connect (s4, "tcp://127.0.0.1:5555");
    zsocket_connect (s5, "tcp://127.0.0.1:5555");
    zsocket_connect (s6, "tcp://127.0.0.1:5555");
    assert (zctx_underlying (ctx));
    zctx_destroy (&ctx);
    //  @end

    printf ("OK\n");
    return 0;
}
