#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INTEL_NO_MACRO_BODY
#define INTEL_ITTNOTIFY_API_PRIVATE
#include "ittnotify.h"
#include "ittnotify_config.h"

#define LOG_BUFFER_MAX_SIZE 256

#define ITT_MUTEX_INIT_AND_LOCK(p) {                                 \
    if (PTHREAD_SYMBOLS)                                             \
    {                                                                \
        if (!p->mutex_initialized)                                    \
        {                                                            \
            if (__itt_interlocked_compare_exchange(&p->atomic_counter, 1, 0) == 0) \
            {                                                        \
                __itt_mutex_init(&p->mutex);                          \
                p->mutex_initialized = 1;                             \
            }                                                        \
            else                                                     \
                while (!p->mutex_initialized)                         \
                    __itt_thread_yield();                            \
        }                                                            \
        __itt_mutex_lock(&p->mutex);                                  \
    }                                                                \
}



static const char* env_log_dir = "INTEL_LIBITTNOTIFY_LOG_DIR";
static const char* log_level_str[] = {"INFO", "WARN", "ERROR", "FATAL_ERROR"};

enum {
    LOG_LVL_INFO,
    LOG_LVL_WARN,
    LOG_LVL_ERROR,
    LOG_LVL_FATAL
};

static struct ref_collector_logger {
    char* file_name;
    uint8_t init_state;
} g_ref_collector_logger = {NULL, 0};

char* log_file_name_generate()
{
    time_t time_now = time(NULL);
    struct tm* time_info = localtime(&time_now);
    char* log_file_name = (char*)malloc(sizeof(char) * (LOG_BUFFER_MAX_SIZE/2));

    sprintf(log_file_name,"libittnotify_refcol_%d%d%d%d%d%d.log",
            time_info->tm_year+1900, time_info->tm_mon+1, time_info->tm_mday,
            time_info->tm_hour, time_info->tm_min, time_info->tm_sec);

    return log_file_name;
}

static __itt_global * itt_global_ptr = NULL;

static void __itt_report_error(int code, ...)
{
    va_list args;
    va_start(args, code);
    if (itt_global_ptr->error_handler != NULL)
    {
        __itt_error_handler_t* handler = (__itt_error_handler_t*)(size_t)itt_global_ptr->error_handler;
        handler((__itt_error_code)code, args);
    }
    va_end(args);
}

void ref_col_init()
{
    if (!g_ref_collector_logger.init_state)
    {
        static char file_name_buffer[LOG_BUFFER_MAX_SIZE*2];
        char* log_dir = getenv(env_log_dir);
        char* log_file = log_file_name_generate();

        if (log_dir != NULL)
        {
            #ifdef _WIN32
                sprintf(file_name_buffer,"%s\\%s", log_dir, log_file);
            #else
                sprintf(file_name_buffer,"%s/%s", log_dir, log_file);
            #endif
        }
        else
        {
            #ifdef _WIN32
                char* temp_dir = getenv("TEMP");
                if (temp_dir != NULL)
                {
                    sprintf(file_name_buffer,"%s\\%s", temp_dir, log_file);
                }
            #else
                sprintf(file_name_buffer,"/tmp/%s", log_file);
            #endif
        }
        free(log_file);

        g_ref_collector_logger.file_name = file_name_buffer;
        g_ref_collector_logger.init_state = 1;
    }
}

static void fill_func_ptr_per_lib(__itt_global* p)
{
    __itt_api_info* api_list = (__itt_api_info*)p->api_list_ptr;

    for (int i = 0; api_list[i].name != NULL; i++)
    {
        *(api_list[i].func_ptr) = (void*)__itt_get_proc(p->lib, api_list[i].name);
        if (*(api_list[i].func_ptr) == NULL)
        {
            *(api_list[i].func_ptr) = api_list[i].null_func;
        }
    }
}

ITT_EXTERN_C void ITTAPI __itt_api_init(__itt_global* p, __itt_group_id init_groups)
{
    if (p != NULL)
    {
        (void)init_groups;
        fill_func_ptr_per_lib(p);
        ref_col_init();
        itt_global_ptr = p;
        fprintf(stderr, "__itt_api_init\n");
    }
    else
    {
        printf("ERROR: Failed to initialize dynamic library\n");
    }
}

void log_func_call(uint8_t log_level, const char* function_name, const char* message_format, ...)
{
    if (g_ref_collector_logger.init_state)
    {
        FILE * pLogFile = NULL;
        char log_buffer[LOG_BUFFER_MAX_SIZE];
        uint32_t result_len = 0;
        va_list message_args;

        result_len += sprintf(log_buffer, "[%s] %s(...) - ", log_level_str[log_level] ,function_name);
        va_start(message_args, message_format);
        vsnprintf(log_buffer + result_len, LOG_BUFFER_MAX_SIZE - result_len, message_format, message_args);

        pLogFile = fopen(g_ref_collector_logger.file_name, "a");
        if (!pLogFile)
        {
            printf("ERROR: Cannot open file: %s\n", g_ref_collector_logger.file_name);
            return;
        }
        fprintf(pLogFile, "%s\n", log_buffer);
        fclose(pLogFile);
    }
    else
    {
        return;
    }
}

#define LOG_FUNC_CALL_INFO(...)  log_func_call(LOG_LVL_INFO, __FUNCTION__, __VA_ARGS__)
#define LOG_FUNC_CALL_WARN(...)  log_func_call(LOG_LVL_WARN, __FUNCTION__, __VA_ARGS__)
#define LOG_FUNC_CALL_ERROR(...) log_func_call(LOG_LVL_ERROR, __FUNCTION__, __VA_ARGS__)
#define LOG_FUNC_CALL_FATAL(...) log_func_call(LOG_LVL_FATAL, __FUNCTION__, __VA_ARGS__)

/* ------------------------------------------------------------------------------ */
/* The code below is a reference implementation of the
/* Instrumentation and Tracing Technology API (ITT API) dynamic collector.
/* This implementation is designed to log ITTAPI functions calls.*/
/* ------------------------------------------------------------------------------ */

/* Please remember to call free() after using get_metadata_elements() */
char* get_metadata_elements(size_t size, __itt_metadata_type type, void* metadata)
{
    char* metadata_str = (char*)malloc(sizeof(char) * LOG_BUFFER_MAX_SIZE);
    *metadata_str = '\0';
    uint16_t offset = 0;

    switch (type)
    {
    case __itt_metadata_u64:
        for (uint16_t i = 0; i < size; i++)
            offset += sprintf(metadata_str + offset, "%lu;", ((uint64_t*)metadata)[i]);
        break;
    case __itt_metadata_s64:
        for (uint16_t i = 0; i < size; i++)
            offset += sprintf(metadata_str + offset, "%ld;", ((int64_t*)metadata)[i]);
        break;
    case __itt_metadata_u32:
        for (uint16_t i = 0; i < size; i++)
            offset += sprintf(metadata_str + offset, "%u;", ((uint32_t*)metadata)[i]);
        break;
    case __itt_metadata_s32:
        for (uint16_t i = 0; i < size; i++)
            offset += sprintf(metadata_str + offset, "%d;", ((int32_t*)metadata)[i]);
        break;
    case __itt_metadata_u16:
        for (uint16_t i = 0; i < size; i++)
            offset += sprintf(metadata_str + offset, "%u;", ((uint16_t*)metadata)[i]);
        break;
    case __itt_metadata_s16:
        for (uint16_t i = 0; i < size; i++)
            offset += sprintf(metadata_str + offset, "%d;", ((int16_t*)metadata)[i]);
        break;
    case __itt_metadata_float:
        for (uint16_t i = 0; i < size; i++)
            offset += sprintf(metadata_str + offset, "%f;", ((float*)metadata)[i]);
        break;
    case __itt_metadata_double:
        for (uint16_t i = 0; i < size; i++)
            offset += sprintf(metadata_str + offset, "%lf;", ((double*)metadata)[i]);
        break;
    default:
        printf("ERROR: Unknow metadata type\n");
        break;
    }

    return metadata_str;
}

/* Please remember to call free() after using get_context_metadata_element() */
char* get_context_metadata_element(__itt_context_type type, void* metadata)
{
    char* metadata_str = (char*)malloc(sizeof(char) * LOG_BUFFER_MAX_SIZE/4);
    *metadata_str = '\0';

    switch(type)
    {
        case __itt_context_name:
        case __itt_context_device:
        case __itt_context_units:
        case __itt_context_pci_addr:
            sprintf(metadata_str, "%s;", ((char*)metadata));
            break;
        case __itt_context_max_val:
        case __itt_context_tid:
        case __itt_context_on_thread_flag:
        case __itt_context_bandwidth_flag:
        case __itt_context_latency_flag:
        case __itt_context_occupancy_flag:
        case __itt_context_cpu_instructions_flag:
        case __itt_context_cpu_cycles_flag:
        case __itt_context_is_abs_val_flag:
            sprintf(metadata_str, "%lu;", *(uint64_t*)metadata);
            break;
        default:
            printf("ERROR: Unknown context metadata type");
            break;
    }

    return metadata_str;
}

ITT_EXTERN_C __itt_string_handle * ITTAPI __itt_string_handle_create(const char *name) 
{
    if(name == NULL) {
        return NULL;
    }

    __itt_string_handle * head = itt_global_ptr->string_list;
    __itt_string_handle * tail = NULL;
    __itt_string_handle * result = NULL;
    
    if(PTHREAD_SYMBOLS) {
        ITT_MUTEX_INIT_AND_LOCK(itt_global_ptr);
    }

    // Search for existing
    
    for(__itt_string_handle * cur = head; cur != NULL; cur = cur->next) {
        tail = cur;
        if(cur->strA != NULL && !__itt_fstrcmp(cur->strA, name)) {
            result = cur;
            fprintf(stderr, "Found string handle %s\n", result->strA);
            break;
        }
    }

    // Create if not existing

    if(result == NULL) {
        NEW_STRING_HANDLE_A(itt_global_ptr, result, tail, name);
        fprintf(stderr, "Placed new string handle %s after %s\n", name, tail->strA);
    }

    LOG_FUNC_CALL_INFO("function args: name=%s", name);

    if(PTHREAD_SYMBOLS) {
        __itt_mutex_unlock(&itt_global_ptr->mutex);
    }

    fprintf(stderr, "Returning string handle %p for %s\n", result, name);
    return result;
}

ITT_EXTERN_C __itt_domain * ITTAPI __itt_domain_create(const char * name) {

    if(name == NULL) {
        return NULL;
    }

    __itt_domain * head = itt_global_ptr->domain_list;
    __itt_domain * tail = NULL;
    __itt_domain * result = NULL;

    if(PTHREAD_SYMBOLS) {
        ITT_MUTEX_INIT_AND_LOCK(itt_global_ptr);
    }

    // Search for existing
    
    for(__itt_domain * cur = head; cur != NULL; cur = cur->next) {
        tail = cur;
        if(cur->nameA != NULL && !__itt_fstrcmp(cur->nameA, name)) {
            result = cur;
            fprintf(stderr, "Found domain %s\n", result->nameA);
            break;
        }
    }

    // Create if not existing

    if(result == NULL) {
        NEW_DOMAIN_A(itt_global_ptr, result, tail, name);
        fprintf(stderr, "Placed new domain %s after %s\n", name, tail->nameA);
    }

    LOG_FUNC_CALL_INFO("function args: name=%s", name);

    if(PTHREAD_SYMBOLS) {
        __itt_mutex_unlock(&itt_global_ptr->mutex);
    }

    fprintf(stderr, "Returning domain %p for %s\n", result, name);
    return result;
}


ITT_EXTERN_C void ITTAPI __itt_pause(void)
{
    LOG_FUNC_CALL_INFO("function call");
}

ITT_EXTERN_C void ITTAPI __itt_pause_scoped(__itt_collection_scope scope)
{
    LOG_FUNC_CALL_INFO("functions args: scope=%d", scope);
}

ITT_EXTERN_C void ITTAPI __itt_resume(void)
{
    LOG_FUNC_CALL_INFO("function call");
}

ITT_EXTERN_C void ITTAPI __itt_resume_scoped(__itt_collection_scope scope)
{
    LOG_FUNC_CALL_INFO("functions args: scope=%d", scope);
}

ITT_EXTERN_C void ITTAPI __itt_detach(void)
{
    LOG_FUNC_CALL_INFO("function call");
}

ITT_EXTERN_C void ITTAPI __itt_frame_begin_v3(const __itt_domain *domain, __itt_id *id)
{
    if (domain != NULL)
    {
        (void)id;
        LOG_FUNC_CALL_INFO("functions args: domain=%s", domain->nameA);
    }
    else
    {
        LOG_FUNC_CALL_WARN("Incorrect function call");
    }
}

ITT_EXTERN_C void ITTAPI __itt_frame_end_v3(const __itt_domain *domain, __itt_id *id)
{
    if (domain != NULL)
    {
        (void)id;
        LOG_FUNC_CALL_INFO("functions args: domain=%s", domain->nameA);
    }
    else
    {
        LOG_FUNC_CALL_WARN("Incorrect function call");
    }
}

ITT_EXTERN_C void ITTAPI __itt_frame_submit_v3(const __itt_domain *domain, __itt_id *id,
    __itt_timestamp begin, __itt_timestamp end)
{
    if (domain != NULL)
    {
        (void)id;
        LOG_FUNC_CALL_INFO("functions args: domain=%s, time_begin=%llu, time_end=%llu",
                        domain->nameA, begin, end);
    }
    else
    {
        LOG_FUNC_CALL_WARN("Incorrect function call");
    }
}

ITT_EXTERN_C void ITTAPI __itt_task_begin(
    const __itt_domain *domain, __itt_id taskid, __itt_id parentid, __itt_string_handle *name)
{
    if (domain != NULL && name != NULL)
    {
        (void)taskid;
        (void)parentid;
        LOG_FUNC_CALL_INFO("functions args: domain=%s handle=%s", domain->nameA, name->strA);
    }
    else
    {
        LOG_FUNC_CALL_WARN("Incorrect function call");
    }
}

ITT_EXTERN_C void ITTAPI __itt_task_end(const __itt_domain *domain)
{
    if (domain != NULL)
    {
        LOG_FUNC_CALL_INFO("functions args: domain=%s", domain->nameA);
    }
    else
    {
        LOG_FUNC_CALL_WARN("Incorrect function call");
    }
}

ITT_EXTERN_C void __itt_metadata_add(const __itt_domain *domain, __itt_id id,
    __itt_string_handle *key, __itt_metadata_type type, size_t count, void *data)
{
    if (domain != NULL && count != 0)
    {
        (void)id;
        (void)key;
        char* metadata_str = get_metadata_elements(count, type, data);
        LOG_FUNC_CALL_INFO("functions args: domain=%s metadata_size=%lu metadata[]=%s",
                            domain->nameA, count, metadata_str);
        free(metadata_str);
    }
    else
    {
        LOG_FUNC_CALL_WARN("Incorrect function call");
    }
}

ITT_EXTERN_C void __itt_formatted_metadata_add(const __itt_domain *domain, __itt_string_handle *format, ...)
{
    if (domain == NULL || format == NULL)
    {
        LOG_FUNC_CALL_WARN("Incorrect function call");
        return;
    }

    va_list args;
    va_start(args, format);

    char formatted_metadata[LOG_BUFFER_MAX_SIZE];
    vsnprintf(formatted_metadata, LOG_BUFFER_MAX_SIZE, format->strA, args);

    LOG_FUNC_CALL_INFO("functions args: domain=%s formatted_metadata=%s",
                        domain->nameA, formatted_metadata);

    va_end(args);
}

ITT_EXTERN_C void __itt_histogram_submit(__itt_histogram* hist, size_t length, void* x_data, void* y_data)
{
    if (hist == NULL)
    {
        LOG_FUNC_CALL_WARN("Histogram is NULL");
    }
    else if (hist->domain == NULL)
    {
        LOG_FUNC_CALL_WARN("Histogram domain is NULL");
    }
    else if (hist->domain->nameA != NULL && hist->nameA != NULL && length != 0 && y_data != NULL)
    {
        if (x_data != NULL)
        {
            char* x_data_str = get_metadata_elements(length, hist->x_type, x_data);
            char* y_data_str = get_metadata_elements(length, hist->y_type, y_data);
            LOG_FUNC_CALL_INFO("functions args: domain=%s name=%s histogram_size=%lu x[]=%s y[]=%s",
                                hist->domain->nameA, hist->nameA, length, x_data_str, y_data_str);
            free(x_data_str);
            free(y_data_str);
        }
        else
        {
            char* y_data_str = get_metadata_elements(length, hist->y_type, y_data);
            LOG_FUNC_CALL_INFO("functions args: domain=%s name=%s histogram_size=%lu y[]=%s",
                                hist->domain->nameA, hist->nameA, length, y_data_str);
            free(y_data_str);
        }
    }
    else
    {
        LOG_FUNC_CALL_WARN("Incorrect function call");
    }
}

ITT_EXTERN_C void __itt_bind_context_metadata_to_counter(__itt_counter counter, size_t length, __itt_context_metadata* metadata)
{
    if (counter != NULL && metadata != NULL && length != 0)
    {
        __itt_counter_info_t* counter_info = (__itt_counter_info_t*)counter;
        char context_metadata[LOG_BUFFER_MAX_SIZE];
        context_metadata[0] = '\0';
        uint16_t offset = 0;
        for(size_t i=0; i<length; i++)
        {
            char* context_metadata_element = get_context_metadata_element(metadata[i].type, metadata[i].value);
            offset += sprintf(context_metadata+ offset, "%s", context_metadata_element);
            free(context_metadata_element);
        }
        LOG_FUNC_CALL_INFO("functions args: counter_name=%s context_metadata_size=%lu context_metadata[]=%s",
                            counter_info->nameA, length, context_metadata);
    }
    else
    {
        LOG_FUNC_CALL_WARN("Incorrect function call");
    }
}

ITT_EXTERN_C void __itt_counter_set_value_v3(__itt_counter counter, void* value_ptr)
{
    if (counter != NULL && value_ptr != NULL)
    {
        __itt_counter_info_t* counter_info = (__itt_counter_info_t*)counter;
        uint64_t value = *(uint64_t*)value_ptr;
        LOG_FUNC_CALL_INFO("functions args: counter_name=%s counter_value=%lu",
                            counter_info->nameA, value);
    }
    else
    {
        LOG_FUNC_CALL_WARN("Incorrect function call");
    }
}
