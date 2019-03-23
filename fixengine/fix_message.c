#include "fix_message.h"

#include "fix_session.h"
#include "read-write.h"
#include "buffer.h"
#include "array.h"
#include "trace.h"
#include "itoa.h"

#include "modp_numtoa.h"

#include <sys/socket.h>
#include <inttypes.h>
#include <sys/uio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

const char *fix_msg_types[FIX_MSG_TYPE_MAX] = {
	[FIX_MSG_TYPE_HEARTBEAT]		= "0",
	[FIX_MSG_TYPE_TEST_REQUEST]		= "1",
	[FIX_MSG_TYPE_RESEND_REQUEST]		= "2",
	[FIX_MSG_TYPE_REJECT]			= "3",
	[FIX_MSG_TYPE_SEQUENCE_RESET]		= "4",
	[FIX_MSG_TYPE_LOGOUT]			= "5",
	[FIX_MSG_TYPE_EXECUTION_REPORT]		= "8",
	[FIX_MSG_ORDER_CANCEL_REJECT]		= "9",
	[FIX_MSG_TYPE_LOGON]			= "A",
	[FIX_MSG_TYPE_NEW_ORDER_SINGLE]		= "D",
	[FIX_MSG_ORDER_CANCEL_REQUEST]		= "F",
	[FIX_MSG_ORDER_CANCEL_REPLACE]		= "G",
	[FIX_MSG_TYPE_SNAPSHOT_REFRESH]		= "W",
	[FIX_MSG_TYPE_INCREMENT_REFRESH]	= "X",
	[FIX_MSG_TYPE_SESSION_STATUS]		= "h",
	[FIX_MSG_TYPE_SECURITY_STATUS]		= "f",
	[FIX_MSG_ORDER_MASS_CANCEL_REQUEST]	= "q",
	[FIX_MSG_ORDER_MASS_CANCEL_REPORT]	= "r",
	[FIX_MSG_QUOTE_REQUEST]			= "R",
	[FIX_MSG_SECURITY_DEFINITION_REQUEST]	= "c",
	[FIX_MSG_NEW_ORDER_CROSS]		= "s",
	[FIX_MSG_MASS_QUOTE]			= "i",
	[FIX_MSG_QUOTE_CANCEL]			= "Z",
	[FIX_MSG_SECURITY_DEFINITION]		= "d",
	[FIX_MSG_QUOTE_ACKNOWLEDGEMENT]		= "b",
	[FIX_MSG_ORDER_MASS_STATUS_REQUEST]	= "AF",
	[FIX_MSG_ORDER_MASS_ACTION_REQUEST]	= "CA",
	[FIX_MSG_ORDER_MASS_ACTION_REPORT]	= "BZ",
};

enum fix_msg_type fix_msg_type_parse(const char *s, const char delim)
{
	if (s[1] != delim) {
		if (s[2] != delim)
			return FIX_MSG_TYPE_UNKNOWN;
		if (s[0] == 'A' && s[1] == 'F') return FIX_MSG_ORDER_MASS_STATUS_REQUEST;
		else if (s[0] == 'C' && s[1] == 'A') return FIX_MSG_ORDER_MASS_ACTION_REQUEST;
		else if (s[0] == 'B' && s[1] == 'Z') return FIX_MSG_ORDER_MASS_ACTION_REPORT;
		else return FIX_MSG_TYPE_UNKNOWN;
	}

	/*
	 * Single-character message type:
	 */
	switch (s[0]) {
	case '0': return FIX_MSG_TYPE_HEARTBEAT;
	case '1': return FIX_MSG_TYPE_TEST_REQUEST;
	case '2': return FIX_MSG_TYPE_RESEND_REQUEST;
	case '3': return FIX_MSG_TYPE_REJECT;
	case '4': return FIX_MSG_TYPE_SEQUENCE_RESET;
	case '5': return FIX_MSG_TYPE_LOGOUT;
	case '8': return FIX_MSG_TYPE_EXECUTION_REPORT;
	case '9': return FIX_MSG_ORDER_CANCEL_REJECT;
	case 'A': return FIX_MSG_TYPE_LOGON;
	case 'D': return FIX_MSG_TYPE_NEW_ORDER_SINGLE;
	case 'F': return FIX_MSG_ORDER_CANCEL_REQUEST;
	case 'G': return FIX_MSG_ORDER_CANCEL_REPLACE;
	case 'W': return FIX_MSG_TYPE_SNAPSHOT_REFRESH;
	case 'X': return FIX_MSG_TYPE_INCREMENT_REFRESH;
	case 'h': return FIX_MSG_TYPE_SESSION_STATUS;
	case 'f': return FIX_MSG_TYPE_SECURITY_STATUS;
	case 'q': return FIX_MSG_ORDER_MASS_CANCEL_REQUEST;
	case 'r': return FIX_MSG_ORDER_MASS_CANCEL_REPORT;
	case 'R': return FIX_MSG_QUOTE_REQUEST;
	case 'c': return FIX_MSG_SECURITY_DEFINITION_REQUEST;
	case 's': return FIX_MSG_NEW_ORDER_CROSS;
	case 'i': return FIX_MSG_MASS_QUOTE;
	case 'Z': return FIX_MSG_QUOTE_CANCEL;
	case 'd': return FIX_MSG_SECURITY_DEFINITION;
	case 'b': return FIX_MSG_QUOTE_ACKNOWLEDGEMENT;
	default : return FIX_MSG_TYPE_UNKNOWN;
	}
}

int64_t fix_atoi64(const char *p, const char **end)
{
	int64_t ret = 0;
	bool neg = false;
	if (*p == '-') {
		neg = true;
		p++;
	}
	while (*p >= '0' && *p <= '9') {
		ret = (ret*10) + (*p - '0');
		p++;
	}
	if (neg) {
		ret = -ret;
	}
	if (end) {
		*end = p;
	}
	return ret;
}

inline int fix_uatoi(const char *p, const char **end)
{
	int ret = 0;
	while (*p >= '0' && *p <= '9') {
		ret = (ret*10) + (*p - '0');
		p++;
	}
	if (end) {
		*end = p;
	}
	return ret;
}

static int parse_tag(struct buffer *self, int *tag)
{
	const char *delim;
	const char *start;
	const char *end;
	int ret;

	start = buffer_start(self);
	delim = buffer_find(self, '=');

	if (!delim || *delim != '=')
		return FIX_MSG_STATE_PARTIAL;
	ret = fix_uatoi(start, &end);
	if (end != delim)
		return FIX_MSG_STATE_GARBLED;

	buffer_advance(self, 1);

	*tag = ret;
	//printf("tag is %d\n", ret);

	return 0;
}

static int parse_value(struct buffer *self, const char **value)
{
	char *start, *end;
	int c = 0;

	start = buffer_start(self);

	end = buffer_find(self, 0x01);

	if (!end || *end != 0x01)
		return FIX_MSG_STATE_PARTIAL;

	buffer_advance(self, 1);
	
	*value = start;
	//printf("value is %s\n", *value);

	return 0;
}

static void next_tag(struct buffer *self)
{
	char *delim;

	delim = buffer_find(self, 0x01);

	if (!delim)
		return;

	if (*delim != 0x01)
		return;

	buffer_advance(self, 1);
}

static int match_field(struct buffer *self, int tag, const char **value)
{
	int ptag, ret;

	ret = parse_tag(self, &ptag);

	if (ret)
		goto fail;
	else if (ptag != tag) {
		ret = FIX_MSG_STATE_GARBLED;
		goto fail;
	}

	return parse_value(self, value);

fail:
	next_tag(self);
	return ret;
}

static int parse_field(struct buffer *self, int *tag, const char **value)
{
	int ret;

	ret = parse_tag(self, tag);

	if (ret)
		goto fail;

	return parse_value(self, value);

fail:
	next_tag(self);
	return ret;
}

static enum fix_type fix_tag_type(int tag)
{
	switch (tag) {
	case CheckSum:			return FIX_TYPE_CHECKSUM;
	case LastMsgSeqNumProcessed:	return FIX_TYPE_INT;
	case MDPriceLevel:		return FIX_TYPE_INT;
	case BeginSeqNo:		return FIX_TYPE_INT;
	case RefSeqNum:			return FIX_TYPE_INT;
	case EndSeqNo:			return FIX_TYPE_INT;
	case NewSeqNo:			return FIX_TYPE_INT;
	case RptSeq:			return FIX_TYPE_INT;
	case GapFillFlag:		return FIX_TYPE_STRING;
	case PossDupFlag:		return FIX_TYPE_STRING;
	case SecurityID:		return FIX_TYPE_STRING;
	case TestReqID:			return FIX_TYPE_STRING;
	case MsgSeqNum:			return FIX_TYPE_MSGSEQNUM;
	case MDEntrySize:		return FIX_TYPE_FLOAT;
	case LastShares:		return FIX_TYPE_FLOAT;
	case LeavesQty:			return FIX_TYPE_FLOAT;
	case MDEntryPx:			return FIX_TYPE_FLOAT;
	case OrderQty:			return FIX_TYPE_FLOAT;
	case CumQty:			return FIX_TYPE_FLOAT;
	case LastPx:			return FIX_TYPE_FLOAT;
	case AvgPx:			return FIX_TYPE_FLOAT;
	case Price:			return FIX_TYPE_FLOAT;
	case TradingSessionID:		return FIX_TYPE_STRING;
	case MDUpdateAction:		return FIX_TYPE_STRING;
	case TransactTime:		return FIX_TYPE_STRING;
	case ExecTransType:		return FIX_TYPE_STRING;
	case OrigClOrdID:		return FIX_TYPE_STRING;
	case MDEntryType:		return FIX_TYPE_STRING;
	case OrdStatus:			return FIX_TYPE_STRING;
	case ExecType:			return FIX_TYPE_STRING;
	case Password:			return FIX_TYPE_STRING;
	case Account:			return FIX_TYPE_STRING;
	case ClOrdID:			return FIX_TYPE_STRING;
	case OrderID:			return FIX_TYPE_STRING;
	case OrdType:			return FIX_TYPE_STRING;
	case ExecID:			return FIX_TYPE_STRING;
	case Symbol:			return FIX_TYPE_STRING;
	case Side:			return FIX_TYPE_STRING;
	case Text:			return FIX_TYPE_STRING;
	case OrdRejReason:		return FIX_TYPE_INT;
	case MultiLegReportingType:	return FIX_TYPE_CHAR;
	case RawData:	    		return FIX_TYPE_STRING;
	default:			return FIX_TYPE_STRING;	/* unrecognized tag */
	}
}

static void rest_of_message(struct fix_message *self, struct fix_dialect *dialect, struct buffer *buffer)
{
	int tag = 0;
	const char *tag_ptr = NULL;
	unsigned long nr_fields = 0;
	enum fix_type type;

	self->nr_fields = 0;

retry:
	if (parse_field(buffer, &tag, &tag_ptr))
		return;

	type = dialect->tag_type(tag);

	switch (type) {
	case FIX_TYPE_INT:
		self->fields[nr_fields++] = FIX_INT_FIELD(tag, fix_atoi64(tag_ptr, NULL));
		goto retry;
	case FIX_TYPE_FLOAT:
		self->fields[nr_fields++] = FIX_FLOAT_FIELD(tag, strtod(tag_ptr, NULL));
		goto retry;
	case FIX_TYPE_CHAR:
		self->fields[nr_fields++] = FIX_CHAR_FIELD(tag, tag_ptr[0]);
		goto retry;
	case FIX_TYPE_STRING:
		self->fields[nr_fields++] = FIX_STRING_FIELD(tag, tag_ptr);
		goto retry;
	case FIX_TYPE_CHECKSUM:
		break;
	case FIX_TYPE_MSGSEQNUM:
		self->msg_seq_num = fix_uatoi(tag_ptr, NULL);
		goto retry;
	default:
		goto retry;
	}

	self->nr_fields = nr_fields;
}

static bool verify_checksum(struct fix_message *self, struct buffer *buffer)
{
	int cksum, actual;

	cksum	= fix_uatoi(self->check_sum, NULL);

	actual	= buffer_sum_range(self->begin_string - 2, self->check_sum - 3);

	return actual == cksum;
}

/*
 * The function assumes that the following patterns have fixed sizes:
 * - "BeginString=" ("8=") is 2 bytes long
 * - "CheckSum=" ("10=") is 3 bytes long
 * - "MsgType=" ("35=") is 3 bytes long
 */
static int checksum(struct fix_message *self, struct buffer *buffer, unsigned long flags)
{
	const char *start;
	int offset;
	int ret;

	start = buffer_start(buffer);

	/* The number of bytes between tag MsgType and buffer's start */
	offset = start - (self->msg_type - 3);

	/*
	 * Checksum tag and its trailing delimiter increase
	 * the message's length by seven bytes - "10=***\x01"
	 */
	if (buffer_size(buffer) + offset < self->body_length + 7) {
		ret = FIX_MSG_STATE_PARTIAL;
		goto exit;
	}

	if (flags & FIX_PARSE_FLAG_NO_CSUM) {
		ret = 0;
		goto exit;
	}

	/* Buffer's start will point to the CheckSum tag */
	buffer_advance(buffer, self->body_length - offset);

	ret = match_field(buffer, CheckSum, &self->check_sum);
	if (ret)
		goto exit;

	if (!verify_checksum(self, buffer)) {
		ret = FIX_MSG_STATE_GARBLED;
		goto exit;
	}

	/* Go back to analyze other fields */
	buffer_advance(buffer, start - buffer_start(buffer));

exit:
	return ret;
}

static int parse_msg_type(struct fix_message *self, unsigned long flags)
{
	int ret;

	ret = match_field(self->head_buf, MsgType, &self->msg_type);

	if (ret)
		goto exit;

	if (!(flags & FIX_PARSE_FLAG_NO_TYPE)) {
		self->type = fix_msg_type_parse(self->msg_type, 0x01);

		/* If third field is not MsgType -> garbled */
		if (fix_message_type_is(self, FIX_MSG_TYPE_UNKNOWN))
			ret = FIX_MSG_STATE_GARBLED;
	} else
		self->type = FIX_MSG_TYPE_UNKNOWN;

exit:
	return ret;
}

static int parse_body_length(struct fix_message *self)
{
	int len, ret;
	const char *ptr;

	ret = match_field(self->head_buf, BodyLength, &ptr);

	if (ret)
		goto exit;

	len = fix_uatoi(ptr, NULL);
	self->body_length = len;

	if (len <= 0 || len > FIX_MAX_MESSAGE_SIZE)
		ret = FIX_MSG_STATE_GARBLED;

exit:
	return ret;
}

static int parse_begin_string(struct fix_message *self)
{
	// if first field is not BeginString -> garbled
	// if BeginString is invalid or empty -> garbled

	return match_field(self->head_buf, BeginString, &self->begin_string);
}

static int first_three_fields(struct fix_message *self, unsigned long flags)
{
	int ret;

	ret = parse_begin_string(self);
	if (ret)
		goto exit;

	ret = parse_body_length(self);
	if (ret)
		goto exit;

	return parse_msg_type(self, flags);

exit:
	return ret;
}

int fix_message_parse(struct fix_message *self, struct fix_dialect *dialect, struct buffer *buffer, unsigned long flags)
{
	const char *start;
	int ret;

	self->head_buf = buffer;

	TRACE(LIBTRADING_FIX_MESSAGE_PARSE(self, dialect, buffer));
retry:
	ret = FIX_MSG_STATE_PARTIAL;

	start	= buffer_start(buffer);

	if (!buffer_size(buffer))
		goto fail;

	ret = first_three_fields(self, flags);
	if (ret)
		goto fail;

	ret = checksum(self, buffer, flags);
	if (ret)
		goto fail;

	rest_of_message(self, dialect, buffer);

	self->iov[0].iov_base	= (void *)start;
	self->iov[0].iov_len 	= buffer_start(buffer) - start;

	TRACE(LIBTRADING_FIX_MESSAGE_PARSE_RET());

	return 0;

fail:
	if (ret != FIX_MSG_STATE_PARTIAL)
		goto retry;

	buffer_advance(buffer, start - buffer_start(buffer));

	TRACE(LIBTRADING_FIX_MESSAGE_PARSE_ERR());

	return -1;
}

int fix_get_field_count(struct fix_message *self) {
	return self->nr_fields;
}

struct fix_field *fix_get_field_at(struct fix_message *self, int i)
{
	return i < self->nr_fields ? &self->fields[i] : NULL;
}

struct fix_field *fix_get_field(struct fix_message *self, int tag)
{
	unsigned long i;

	for (i = 0; i < self->nr_fields; i++) {
		if (self->fields[i].tag == tag)
			return &self->fields[i];
	}

	return NULL;
}

void fix_message_validate(struct fix_message *self)
{
	// if MsgSeqNum is missing -> logout, terminate
}

const char *fix_get_string(struct fix_field *field, char *buffer, unsigned long len)
{
	unsigned long count;
	const char *start, *end;

	start	= field->string_value;

	end	= memchr(start, 0x01, len);

	if (!end)
		return NULL;

	count	= end - start;

	if (len < count)
		return NULL;

	strncpy(buffer, start, count);

	buffer[count] = '\0';

	return buffer;
}

double fix_get_float(struct fix_message *self, int tag, double _default_) {
	struct fix_field *field = fix_get_field(self, tag);
	return field ? field->float_value : _default_;
}

int64_t fix_get_int(struct fix_message *self, int tag, int64_t _default_)
{
	struct fix_field *field = fix_get_field(self, tag);
	return field ? field->int_value : _default_;
}

char fix_get_char(struct fix_message *self, int tag, char _default_)
{
	struct fix_field *field = fix_get_field(self, tag);
	return field ? field->char_value : _default_;
}

struct fix_message *fix_message_new(void)
{
	struct fix_message *self = calloc(1, sizeof *self);

	if (!self)
		return NULL;

	self->fields = calloc(FIX_MAX_FIELD_NUMBER, sizeof(struct fix_field));
	if (!self->fields) {
		fix_message_free(self);
		return NULL;
	}

	return self;
}

void fix_message_free(struct fix_message *self)
{
	if (!self)
		return;

	free(self->fields);
	free(self);
}

void fix_message_add_field(struct fix_message *self, struct fix_field *field)
{
	if (self->nr_fields < FIX_MAX_FIELD_NUMBER) {
		self->fields[self->nr_fields] = *field;
		self->nr_fields++;
	}
}

bool fix_message_type_is(struct fix_message *self, enum fix_msg_type type)
{
	return self->type == type;
}

bool fix_field_unparse(struct fix_field *self, struct buffer *buffer)
{
	buffer->end += uitoa(self->tag, buffer_end(buffer));

	buffer_put(buffer, '=');

	switch (self->type) {
	case FIX_TYPE_STRING: {
		const char *p = self->string_value;
		//printf("%s\n", p);

		while (*p) {
			buffer_put(buffer, *p++);
		}
		break;
	}
	case FIX_TYPE_STRING_8: {
		for (int i = 0; i < sizeof(self->string_8_value) && self->string_8_value[i]; ++i) {
			buffer_put(buffer, self->string_8_value[i]);
		}
		break;
	}
	case FIX_TYPE_CHAR: {
		buffer_put(buffer, self->char_value);
		break;
	}
	case FIX_TYPE_FLOAT: {
		// dtoa2 do not print leading zeros or .0, 7 digits needed sometimes
		buffer->end += modp_dtoa2(self->float_value, buffer_end(buffer), 7);
		break;
	}
	case FIX_TYPE_INT: {
		buffer->end += i64toa(self->int_value, buffer_end(buffer));
		break;
	}
	case FIX_TYPE_CHECKSUM: {
		buffer->end += checksumtoa(self->int_value, buffer_end(buffer));
		break;
	}
	default:
		break;
	};

	buffer_put(buffer, 0x01);

	return true;
}

void fix_message_unparse(struct fix_message *self)
{
	struct fix_field sender_comp_id;
	struct fix_field target_comp_id;
	struct fix_field begin_string;
	struct fix_field sending_time;
	struct fix_field body_length;
	struct fix_field msg_seq_num;
	struct fix_field check_sum;
	struct fix_field msg_type;
	unsigned long cksum;
	char buf[64];
	int i;

	TRACE(LIBTRADING_FIX_MESSAGE_UNPARSE(self));

	strncpy(buf, self->str_now, sizeof(buf));

	/* standard header */
	msg_type	= (self->type != FIX_MSG_TYPE_UNKNOWN) ?
			FIX_STRING_FIELD(MsgType, fix_msg_types[self->type]) :
			FIX_STRING_FIELD(MsgType, self->msg_type);
	sender_comp_id	= FIX_STRING_FIELD(SenderCompID, self->sender_comp_id);
	target_comp_id	= FIX_STRING_FIELD(TargetCompID, self->target_comp_id);
	msg_seq_num	= FIX_INT_FIELD   (MsgSeqNum, self->msg_seq_num);
	sending_time	= FIX_STRING_FIELD(SendingTime, buf);

	/* body */
	fix_field_unparse(&msg_type, self->body_buf);
	fix_field_unparse(&sender_comp_id, self->body_buf);
	fix_field_unparse(&target_comp_id, self->body_buf);
	fix_field_unparse(&msg_seq_num, self->body_buf);
	fix_field_unparse(&sending_time, self->body_buf);

	for (i = 0; i < self->nr_fields; i++)
		fix_field_unparse(&self->fields[i], self->body_buf);

	/* head */
	begin_string	= FIX_STRING_FIELD(BeginString, self->begin_string);
	body_length	= FIX_INT_FIELD(BodyLength, buffer_size(self->body_buf));

	fix_field_unparse(&begin_string, self->head_buf);
	fix_field_unparse(&body_length, self->head_buf);

	/* trailer */
	cksum		= buffer_sum(self->head_buf) + buffer_sum(self->body_buf);
	check_sum	= FIX_CHECKSUM_FIELD(CheckSum, cksum % 256);
	fix_field_unparse(&check_sum, self->body_buf);

	TRACE(LIBTRADING_FIX_MESSAGE_UNPARSE_RET());
}

int fix_message_send(struct fix_message *self, int sockfd, int flags)
{
	size_t msg_size;
	int ret = 0;

	TRACE(LIBTRADING_FIX_MESSAGE_SEND(self, sockfd, flags));

	if (!(flags & FIX_SEND_FLAG_PRESERVE_BUFFER))
		fix_message_unparse(self);

	buffer_to_iovec(self->head_buf, &self->iov[0]);
	buffer_to_iovec(self->body_buf, &self->iov[1]);
	
	//printf("%s\n", self->head_buf->data);
	//printf("%s\n", self->body_buf->data);
	ret = io_sendmsg(sockfd, self->iov, 2, 0);

	msg_size = fix_message_size(self);

	if (!(flags & FIX_SEND_FLAG_PRESERVE_BUFFER)) {
		self->head_buf = self->body_buf = NULL;
	}

	TRACE(LIBTRADING_FIX_MESSAGE_SEND_RET());

	if (ret >= 0)
		return msg_size - ret;
	else
		return ret;
}

struct fix_dialect fix_dialects[] = {
	[FIXT_1_1] = {
		.version	= FIXT_1_1,
		.tag_type	= fix_tag_type,
	},
	[FIX_4_4] = {
		.version	= FIX_4_4,
		.tag_type	= fix_tag_type,
	},
	[FIX_4_3] = {
		.version	= FIX_4_3,
		.tag_type	= fix_tag_type,
	},
	[FIX_4_2] = {
		.version	= FIX_4_2,
		.tag_type	= fix_tag_type,
	},
	[FIX_4_1] = {
		.version	= FIX_4_1,
		.tag_type	= fix_tag_type,
	},
	[FIX_4_0] = {
		.version	= FIX_4_0,
		.tag_type	= fix_tag_type,
	},

};