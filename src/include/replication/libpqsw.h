/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * ---------------------------------------------------------------------------------------
 * libpqsw.h
 *        libpqsw operator module.
 * 
 * 
 * IDENTIFICATION
 *        src/include/replication/libpqsw.h
 *
 * ---------------------------------------------------------------------------------------
 */
#ifndef LIBPQSW_H
#define LIBPQSW_H
#include "postgres.h"
#include "c.h"

#define MAXCONNINFO 1024

class RedirectManager;

#ifdef _cplusplus
extern "C" {
#endif

void DestroyStringInfo(StringInfo str);
/* process msg from backend */
bool libpqsw_process_message(int qtype, const StringInfo msg);
/* process P type msg, true if need redirect*/
bool libpqsw_process_parse_message(const char* commandTag, List* query_list);
/* process Q type msg, true if need in redirect mode*/
bool libpqsw_process_query_message(const char* commandTag, List* query_list, const char* query_string,
    size_t query_string_len = 0);
/* is need send ready_for_query messge to front, if in redirect then false*/
bool libpqsw_need_end();
/* udpate if need ready_for_query messge flag */
void libpqsw_set_end(bool is_end);
/* query if enable redirect*/
bool libpqsw_redirect();
/* udpate redirect flag */
void libpqsw_set_redirect(bool redirect);
//Judge if enable remote_excute.
bool enable_remote_excute();
/* query if enable set command*/
bool libpqsw_get_set_command();
/* if skip readonly check in P or Q message */
bool libpqsw_skip_check_readonly();
/* get unique redirect manager*/
RedirectManager* get_redirect_manager();
#ifdef _cplusplus
}
#endif


// default is output log.
#define LIBPQSW_ENABLE_LOG 1
#define LIBPQSW_DEFAULT_LOG_LEVEL LOG

#define libpqsw_log_enable()    (get_redirect_manager()->log_enable())
#if LIBPQSW_ENABLE_LOG
#define libpqsw_trace(fmt, ...) (get_redirect_manager()->logtrace(LIBPQSW_DEFAULT_LOG_LEVEL, fmt, ##__VA_ARGS__))
#define libpqsw_info(fmt, ...) (get_redirect_manager()->logtrace(LOG, fmt, ##__VA_ARGS__))
#define libpqsw_warn(fmt, ...) (get_redirect_manager()->logtrace(WARNING, fmt, ##__VA_ARGS__))
#else
#define libpqsw_trace(fmt, ...)
#define libpqsw_info(fmt, ...)
#define libpqsw_warn(fmt, ...)
#endif

typedef struct {
    bool inited;
    /* if enable remote excute*/
    bool enable_remote_excute;
    /* if open transaction */
    bool transaction;
    /* if open batch mode */
    bool batch;
    /*if set command*/
    bool set_command;
    /* if need to send master */
    bool redirect;
    /* if need ready_for_query message to front*/
    bool need_end;
    /* if connected to master*/
    bool already_connected;
} RedirectState;

// the max len =(PBEPBEDS) == 8, 20 is enough
#define PBE_MESSAGE_STACK (20)
#define PBE_MESSAGE_MERGE_ID (PBE_MESSAGE_STACK - 1)
#define PBE_MAX_SET_BLOCK (10)
enum RedirectType {
    RT_NORMAL, //transfer to standby
    RT_SET  //not transfer to standby
};

typedef struct {
    int pbe_types[PBE_MESSAGE_STACK];
    StringInfo pbe_stack_msgs[PBE_MESSAGE_STACK];
    int cur_pos;
    RedirectType type;
    char commandTag[COMPLETION_TAG_BUFSIZE];
} RedirectMessage;

class RedirectMessageManager {
public:
    RedirectMessageManager()
    {
        messages = NULL;
        last_message = 0;
    }

    ~RedirectMessageManager()
    {
        reset();
    }
    
    void reset() {
        foreach_cell(message, messages) {
            free_redirect_message((RedirectMessage*)lfirst(message));
        }
        list_free(messages);
        messages = NULL;
        last_message = 0;
    }

    // create a empty message struct
    static RedirectMessage* create_redirect_message(RedirectType msg_type);

    // free a empty message struct
    static void free_redirect_message(RedirectMessage* msg)
    {
        for (int i = 0; i < PBE_MESSAGE_STACK; i++) {
            DestroyStringInfo(msg->pbe_stack_msgs[i]);
        }
        pfree(msg);
    }
    
    void push_message(int qtype, StringInfo msg, bool need_switch, RedirectType msg_type);
    
    bool lots_of_message()
    {
        return list_length(messages) == PBE_MAX_SET_BLOCK;
    }

    // is pre last message S or Q
    bool pre_last_message()
    {
        if (message_empty()) {
            return true;
        }
        return (last_message == 'S' || last_message == 'Q');
    }

    static bool message_overflow(const RedirectMessage* msg)
    {
        return msg->cur_pos == PBE_MESSAGE_MERGE_ID;
    }

    bool message_empty()
    {
        return list_length(messages) == 0;
    }

    const StringInfo get_merge_message(RedirectMessage* msg);
    
    void output_messages(StringInfo output, RedirectMessage* msg) const;

    List* get_messages()
    {
        return messages;
    }
private:
    List* messages;
    int last_message;
};

class RedirectManager : public BaseObject {
public:
    RedirectManager()
    {
        log_trace_msg = makeStringInfo();
        state.transaction = false;
        state.enable_remote_excute = false;
        state.redirect = false;
        state.batch = false;
        state.set_command = false;
        state.inited = false;
        state.need_end = true;
        state.already_connected = false;
    }
    
    bool push_message(int qtype, StringInfo msg, bool need_switch, RedirectType msg_type)
    {
        // if one msg have many sql like 'set a;set b;set c', don't switch
        if (need_switch && !messages_manager.pre_last_message()) {
            need_switch = false;
        }
        messages_manager.push_message(qtype, msg, need_switch, msg_type);
        if (qtype == 'S' || qtype == 'Q') {
            return state.already_connected || messages_manager.lots_of_message();
        }
        return false;
    }

    bool get_remote_excute()
    {
        if (state.inited) {
            return state.enable_remote_excute;
        }
        state.inited = true;
        state.enable_remote_excute = enable_remote_excute();
        return state.enable_remote_excute;
    }

    bool log_enable();

    void logtrace(int level, const char* fmt, ...)
    {
        if (!log_enable()) {
            return;
        }
        if (fmt != log_trace_msg->data) {
            va_list args;
            (void)va_start(args, fmt);
            // This place just is the message print. So there is't need check the value of vsnprintf_s function return. if
            // checked, when the message lengtn is over than log_trace_msg->maxlen, will be abnormal exit.
            (void)vsnprintf_s(log_trace_msg->data, log_trace_msg->maxlen, log_trace_msg->maxlen - 1, fmt, args);
            va_end(args);
        }
        ereport(level, (errmsg("libpqsw:%s", log_trace_msg->data)));
    }
    
    virtual ~RedirectManager()
    {
        DestroyStringInfo(log_trace_msg);
    }
public:
    RedirectState state;
    RedirectMessageManager messages_manager;
private:
    StringInfo log_trace_msg;
};

#endif
