#include <config.h>

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <dbus-gmain.h>
#include <gvfsreadchannel.h>
#include <gio/ginputstreamsocket.h>
#include <gio/goutputstreamsocket.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobcloseread.h>
#include <gvfsjobclosewrite.h>

static void g_vfs_channel_job_source_iface_init (GVfsJobSourceIface *iface);

G_DEFINE_TYPE_WITH_CODE (GVfsChannel, g_vfs_channel, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_VFS_TYPE_JOB_SOURCE,
						g_vfs_channel_job_source_iface_init))

/* TODO: Real P_() */
#define P_(_x) (_x)

enum {
  PROP_0,
  PROP_BACKEND
};

typedef struct
{
  GVfsChannel *channel;
  GInputStream *command_stream;
  char buffer[G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE];
  int buffer_size;
  char *data;
  gsize data_len;
  gsize data_pos;
} RequestReader;

struct _GVfsChannelPrivate
{
  GVfsBackend *backend;
  gboolean connection_closed;
  GInputStream *command_stream;
  GOutputStream *reply_stream;
  int remote_fd;
  
  GVfsBackendHandle backend_handle;
  GVfsJob *current_job;
  guint32 current_job_seq_nr;

  RequestReader *request_reader;
  
  char reply_buffer[G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE];
  int reply_buffer_pos;
  
  char *output_data; /* Owned by job */
  gsize output_data_size;
  gsize output_data_pos;
};

static void start_request_reader       (GVfsChannel  *channel);
static void g_vfs_channel_get_property (GObject      *object,
					guint         prop_id,
					GValue       *value,
					GParamSpec   *pspec);
static void g_vfs_channel_set_property (GObject      *object,
					guint         prop_id,
					const GValue *value,
					GParamSpec   *pspec);


static void
g_vfs_channel_finalize (GObject *object)
{
  GVfsChannel *channel;

  channel = G_VFS_CHANNEL (object);

  if (channel->priv->current_job)
    g_object_unref (channel->priv->current_job);
  channel->priv->current_job = NULL;
  
  if (channel->priv->reply_stream)
    g_object_unref (channel->priv->reply_stream);
  channel->priv->reply_stream = NULL;

  if (channel->priv->request_reader)
    channel->priv->request_reader->channel = NULL;
  channel->priv->request_reader = NULL;

  if (channel->priv->command_stream)
    g_object_unref (channel->priv->command_stream);
  channel->priv->command_stream = NULL;
  
  if (channel->priv->remote_fd != -1)
    close (channel->priv->remote_fd);

  if (channel->priv->backend)
    g_object_unref (channel->priv->backend);
  
  g_assert (channel->priv->backend_handle == NULL);
  
  if (G_OBJECT_CLASS (g_vfs_channel_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_channel_parent_class)->finalize) (object);
}

static void
g_vfs_channel_job_source_iface_init (GVfsJobSourceIface *iface)
{
}

static void
g_vfs_channel_class_init (GVfsChannelClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsChannelPrivate));
  
  gobject_class->finalize = g_vfs_channel_finalize;
  gobject_class->set_property = g_vfs_channel_set_property;
  gobject_class->get_property = g_vfs_channel_get_property;

  g_object_class_install_property (gobject_class,
				   PROP_BACKEND,
				   g_param_spec_object ("backend",
							P_("Backend"),
							P_("Backend implementation to use"),
							G_VFS_TYPE_BACKEND,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
}

static void
g_vfs_channel_init (GVfsChannel *channel)
{
  int socket_fds[2];
  int ret;
  
  channel->priv = G_TYPE_INSTANCE_GET_PRIVATE (channel,
					       G_VFS_TYPE_CHANNEL,
					       GVfsChannelPrivate);
  channel->priv->remote_fd = -1;

  ret = socketpair (AF_UNIX, SOCK_STREAM, 0, socket_fds);
  if (ret == -1) 
    g_warning ("Error creating socket pair: %d\n", errno);
  else
    {
      channel->priv->command_stream = g_input_stream_socket_new (socket_fds[0], TRUE);
      channel->priv->reply_stream = g_output_stream_socket_new (socket_fds[0], FALSE);
      channel->priv->remote_fd = socket_fds[1];
      
      start_request_reader (channel);
    }
}

static void
g_vfs_channel_set_property (GObject         *object,
			    guint            prop_id,
			    const GValue    *value,
			    GParamSpec      *pspec)
{
  GVfsChannel *channel = G_VFS_CHANNEL (object);
  
  switch (prop_id)
    {
    case PROP_BACKEND:
      if (channel->priv->backend)
	g_object_unref (channel->priv->backend);
      channel->priv->backend = G_VFS_BACKEND (g_value_dup_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_channel_get_property (GObject    *object,
			    guint       prop_id,
			    GValue     *value,
			    GParamSpec *pspec)
{
  GVfsChannel *channel = G_VFS_CHANNEL (object);
  
  switch (prop_id)
    {
    case PROP_BACKEND:
      g_value_set_object (value, channel->priv->backend);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_channel_connection_closed (GVfsChannel *channel)
{
  GVfsChannelClass *class;

  if (channel->priv->connection_closed)
    return;
  channel->priv->connection_closed = TRUE;
  
  if (channel->priv->current_job == NULL &&
      channel->priv->backend_handle != NULL)
    {
      class = G_VFS_CHANNEL_GET_CLASS (channel);
      
      channel->priv->current_job = class->close (channel);
      channel->priv->current_job_seq_nr = 0;
      g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (channel), channel->priv->current_job);
    }
  /* Otherwise we'll close when current_job is finished */
}

static void
request_reader_free (RequestReader *reader)
{
  g_object_unref (reader->command_stream);
  g_free (reader->data);
  g_free (reader);
}

/* Ownership of data is passed here to avoid copying it */
static void
got_request (GVfsChannel *channel,
	     GVfsDaemonSocketProtocolRequest *request,
	     gpointer data, gsize data_len)
{
  GVfsChannelClass *class;
  GVfsJob *job;
  GError *error;
  guint32 seq_nr, command, arg1, arg2;
  
  command = g_ntohl (request->command);
  arg1 = g_ntohl (request->arg1);
  arg2 = g_ntohl (request->arg2);
  seq_nr = g_ntohl (request->seq_nr);

  job = NULL;
  
  if (channel->priv->current_job != NULL)
    {
      if (command != G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL)
	{
	  g_warning ("Ignored non-cancel request with outstanding request");
	  /* Can't send an error reply now, that would confuse the reply
	     to the outstanding request */
	}
      else if (arg1 == channel->priv->current_job_seq_nr)
	g_vfs_job_cancel (channel->priv->current_job);
      
      g_free (data);
      return;
    }
  /* Ignore cancel with no outstanding job */
  else if (command != G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL)
    {
      class = G_VFS_CHANNEL_GET_CLASS (channel);

      error = NULL;
      job = class->handle_request (channel,
				   command, seq_nr,
				   arg1, arg2,
				   data, data_len, 
				   &error);
      if (job)
	{
	  channel->priv->current_job = job;
	  channel->priv->current_job_seq_nr = seq_nr;
	  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (channel), channel->priv->current_job);
	}
      else
	{
	  g_vfs_channel_send_error (channel, error);
	  g_error_free (error);
	}
    }
}

static void command_read_cb (GInputStream *input_stream,
			     void         *buffer,
			     gsize         count_requested,
			     gssize        count_read,
			     gpointer      data,
			     GError       *error);


static void
finish_request (RequestReader *reader)
{
  /* Ownership of reader->data passed here */
  got_request (reader->channel, (GVfsDaemonSocketProtocolRequest *)reader->buffer,
	       reader->data, reader->data_len);
  reader->data = NULL;
  
  /* Request more commands immediately, so can get cancel requests */

  reader->buffer_size = 0;
  reader->data_len = 0;
  g_input_stream_read_async (reader->command_stream,
			     reader->buffer + reader->buffer_size,
			     G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE - reader->buffer_size,
			     0,
			     command_read_cb,
			     reader,
			     NULL);
}

static void
data_read_cb (GInputStream *input_stream,
	      void         *buffer,
	      gsize         count_requested,
	      gssize        count_read,
	      gpointer      data,
	      GError       *error)
{
  RequestReader *reader = data;

  if (reader->channel == NULL)
    {
      /* Channel was finalized */
      request_reader_free (reader);
      return;
    }

  if (count_read <= 0)
    {
      reader->channel->priv->request_reader = NULL;
      g_vfs_channel_connection_closed (reader->channel);
      request_reader_free (reader);
      return;
    }

  reader->data_pos += count_read;

  if (reader->data_pos < reader->data_len)
    {
      g_input_stream_read_async (reader->command_stream,
				 reader->data + reader->data_pos,
				 reader->data_len - reader->data_pos,
				 0,
				 data_read_cb,
				 reader,
				 NULL);
      return;
    }
  
  finish_request (reader);
}
  

static void
command_read_cb (GInputStream *input_stream,
		 void         *buffer,
		 gsize         count_requested,
		 gssize        count_read,
		 gpointer      data,
		 GError       *error)
{
  RequestReader *reader = data;
  GVfsDaemonSocketProtocolRequest *request;
  guint32 data_len;

  if (reader->channel == NULL)
    {
      /* Channel was finalized */
      request_reader_free (reader);
      return;
    }
  
  if (count_read <= 0)
    {
      reader->channel->priv->request_reader = NULL;
      g_vfs_channel_connection_closed (reader->channel);
      request_reader_free (reader);
      return;
    }

  reader->buffer_size += count_read;

  if (reader->buffer_size < G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE)
    {
      g_input_stream_read_async (reader->command_stream,
				 reader->buffer + reader->buffer_size,
				 G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE - reader->buffer_size,
				 0,
				 command_read_cb,
				 reader,
				 NULL);
      return;
    }

  request = (GVfsDaemonSocketProtocolRequest *)reader->buffer;
  data_len  = g_ntohl (request->data_len);

  if (data_len > 0)
    {
      reader->data = g_malloc (data_len);
      reader->data_len = data_len;
      reader->data_pos = 0;

      g_input_stream_read_async (reader->command_stream,
				 reader->data + reader->data_pos,
				 reader->data_len - reader->data_pos,
				 0,
				 data_read_cb,
				 reader,
				 NULL);
      return;
    }
  
  finish_request (reader);
}

static void
start_request_reader (GVfsChannel *channel)
{
  RequestReader *reader;

  reader = g_new0 (RequestReader, 1);
  reader->channel = channel;
  reader->command_stream = g_object_ref (channel->priv->command_stream);
  
  g_input_stream_read_async (reader->command_stream,
			     reader->buffer + reader->buffer_size,
			     G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE - reader->buffer_size,
			     0,
			     command_read_cb,
			     reader,
			     NULL);

  channel->priv->request_reader = reader;
}

static void
send_reply_cb (GOutputStream *output_stream,
	       void          *buffer,
	       gsize          bytes_requested,
	       gssize         bytes_written,
	       gpointer       data,
	       GError        *error)
{
  GVfsChannel *channel = data;
  GVfsChannelClass *class;
  GVfsJob *job;

  if (bytes_written <= 0)
    {
      g_vfs_channel_connection_closed (channel);
      goto error_out;
    }

  if (channel->priv->reply_buffer_pos < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
    {
      channel->priv->reply_buffer_pos += bytes_written;

      /* Write more of reply header if needed */
      if (channel->priv->reply_buffer_pos < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
	{
	  g_output_stream_write_async (channel->priv->reply_stream,
				       channel->priv->reply_buffer + channel->priv->reply_buffer_pos,
				       G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE - channel->priv->reply_buffer_pos,
				       0,
				       send_reply_cb, channel,
				       NULL);  
	  return;
	}
      bytes_written = 0;
    }

  channel->priv->output_data_pos += bytes_written;

  /* Write more of output_data if needed */
  if (channel->priv->output_data != NULL &&
      channel->priv->output_data_pos < channel->priv->output_data_size)
    {
      g_output_stream_write_async (channel->priv->reply_stream,
				   channel->priv->output_data + channel->priv->output_data_pos,
				   channel->priv->output_data_size - channel->priv->output_data_pos,
				   0,
				   send_reply_cb, channel,
				   NULL);
      return;
    }

 error_out:
  
  /* Sent full reply */
  channel->priv->output_data = NULL;

  job = channel->priv->current_job;
  channel->priv->current_job = NULL;
  g_vfs_job_emit_finished (job);

  if (G_IS_VFS_JOB_CLOSE_READ (job) ||
      G_IS_VFS_JOB_CLOSE_WRITE (job))
    {
      g_vfs_job_source_closed (G_VFS_JOB_SOURCE (channel));
      channel->priv->backend_handle = NULL;
    }
  else if (channel->priv->connection_closed)
    {
      class = G_VFS_CHANNEL_GET_CLASS (channel);

      channel->priv->current_job = class->close (channel);
      channel->priv->current_job_seq_nr = 0;
      g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (channel), channel->priv->current_job);
    }

  g_object_unref (job);
  g_print ("Sent reply\n");
}

/* Might be called on an i/o thread */
void
g_vfs_channel_send_reply (GVfsChannel *channel,
			  GVfsDaemonSocketProtocolReply *reply,
			  void *data,
			  gsize data_len)
{
  
  channel->priv->output_data = data;
  channel->priv->output_data_size = data_len;
  channel->priv->output_data_pos = 0;

  if (reply != NULL)
    {
      memcpy (channel->priv->reply_buffer, reply, sizeof (GVfsDaemonSocketProtocolReply));
      channel->priv->reply_buffer_pos = 0;

      g_output_stream_write_async (channel->priv->reply_stream,
				   channel->priv->reply_buffer,
				   G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE,
				   0,
				   send_reply_cb, channel,
				   NULL);  
    }
  else
    {
      channel->priv->reply_buffer_pos = G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE;
      g_output_stream_write_async (channel->priv->reply_stream,
				   channel->priv->output_data,
				   channel->priv->output_data_size,
				   0,
				   send_reply_cb, channel,
				   NULL);  
    }
}

/* Might be called on an i/o thread
 */
void
g_vfs_channel_send_error (GVfsChannel *channel,
			  GError *error)
{
  char *data;
  gsize data_len;
  
  data = g_error_to_daemon_reply (error, channel->priv->current_job_seq_nr, &data_len);
  g_vfs_channel_send_reply (channel, NULL, data, data_len);
}

int
g_vfs_channel_steal_remote_fd (GVfsChannel *channel)
{
  int fd;
  fd = channel->priv->remote_fd;
  channel->priv->remote_fd = -1;
  return fd;
}

GVfsBackend *
g_vfs_channel_get_backend (GVfsChannel  *channel)
{
  return channel->priv->backend;
}

void
g_vfs_channel_set_backend_handle (GVfsChannel *channel,
				  GVfsBackendHandle backend_handle)
{
  channel->priv->backend_handle = backend_handle;
}

GVfsBackendHandle
g_vfs_channel_get_backend_handle (GVfsChannel *channel)
{
  return channel->priv->backend_handle;
}

guint32
g_vfs_channel_get_current_seq_nr (GVfsChannel *channel)
{
  return channel->priv->current_job_seq_nr;
}
