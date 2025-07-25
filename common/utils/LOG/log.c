/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file log.c
* \brief log implementaion
* \author Navid Nikaein
* \date 2009 - 2014
* \version 0.5
* @ingroup util

*/

#define _GNU_SOURCE  /* required for pthread_getname_np */
//#define LOG_TEST 1

#define COMPONENT_LOG
#define COMPONENT_LOG_IF
#include <ctype.h>
#define LOG_MAIN
#include "log.h"
#include "vcd_signal_dumper.h"
#include "assertions.h"

#include <pthread.h>
#include <string.h>
#include <linux/prctl.h>
#include "common/config/config_userapi.h"
#include <time.h>
#include <sys/time.h>
#include <stdatomic.h>
#include "common/utils/LOG/log.h"

#define LOG_MEM_SIZE 100*1024*1024

// main log variables

/** @defgroup _max_length Maximum Length of LOG
 *  @ingroup _macro
 *  @brief the macros that describe the maximum length of LOG
 * @{*/

#define MAX_LOG_TOTAL 16384 /*!< \brief the maximum length of a log */

// Fixme: a better place to be shure it is called 
void read_cpu_hardware (void) __attribute__ ((constructor));
#if !defined(__arm__) && !defined(__aarch64__) 
  void read_cpu_hardware (void) {__builtin_cpu_init(); }
#else 
  void read_cpu_hardware (void) {}
#endif

  
typedef struct {
  char* buf_p;
  int buf_index;
  int enable_flag;
} log_mem_cnt_t;

static log_mem_cnt_t log_mem_d[2];
static int log_mem_flag = 0;
static volatile int log_mem_side = 0;
static pthread_mutex_t log_mem_lock;
static pthread_cond_t log_mem_notify;
static pthread_t log_mem_thread;
static int log_mem_file_cnt=0;
static volatile int log_mem_write_flag = 0;
static volatile int log_mem_write_side = 0;
static char * log_mem_filename;

static mapping log_level_names[] = {{"error", OAILOG_ERR},
                                   {"warn", OAILOG_WARNING},
                                   {"analysis", OAILOG_ANALYSIS},
                                   {"info", OAILOG_INFO},
                                   {"debug", OAILOG_DEBUG},
                                   {"trace", OAILOG_TRACE},
                                   {NULL, -1}};
mapping * log_level_names_ptr(void)
{
  return log_level_names;
}


/** @defgroup _log_format Defined log format
 *  @ingroup _macro
 *  @brief Macro of log formats defined by LOG
 * @{*/
static const unsigned int FLAG_NOCOLOR = 1; /*!< \brief use colors in log messages, depending on level */
static const unsigned int FLAG_THREAD = 1 << 1; /*!< \brief display thread name in log messages */
static const unsigned int FLAG_LEVEL = 1 << 2; /*!< \brief display log level in log messages */
static const unsigned int FLAG_FUNCT = 1 << 3;
static const unsigned int FLAG_FILE_LINE = 1 << 4;
static const unsigned int FLAG_TIME = 1 << 5;
static const unsigned int FLAG_THREAD_ID = 1 << 6;
static const unsigned int FLAG_REAL_TIME = 1 << 7;
static const unsigned int FLAG_UTC_TIME = 1 << 8;
static const unsigned int FLAG_INITIALIZED = 1 << 9;

/** @}*/
static mapping log_options[] = {{"nocolor", FLAG_NOCOLOR},
                                      {"level", FLAG_LEVEL},
                                      {"thread", FLAG_THREAD},
                                      {"line_num", FLAG_FILE_LINE},
                                      {"function", FLAG_FUNCT},
                                      {"time", FLAG_TIME},
                                      {"thread_id", FLAG_THREAD_ID},
                                      {"wall_clock", FLAG_REAL_TIME},
                                      {"utc_time", FLAG_UTC_TIME},
                                      {NULL, -1}};
mapping * log_option_names_ptr(void)
{
  return log_options;
}

/* .log_format = 0x13 uncolored standard messages
 * .log_format = 0x93 colored standard messages */
/* keep white space in first position; switching it to 0 allows colors to be disabled*/
static const char *const LOG_RED = "\033[1;31m"; /*!< \brief VT100 sequence for bold red foreground */
static const char *const LOG_GREEN = "\033[32m"; /*!< \brief VT100 sequence for green foreground */
static const char *const LOG_ORANGE = "\033[93m"; /*!< \brief VT100 sequence for orange foreground */
static const char *const LOG_BLUE = "\033[34m"; /*!< \brief VT100 sequence for blue foreground */
static const char *const LOG_CYBL = "\033[40;36m"; /*!< \brief VT100 sequence for cyan foreground on black background */
static const char *const LOG_RESET = "\033[0m"; /*!< \brief VT100 sequence for reset (black) foreground */
static const char *const log_level_highlight_start[] =
    {LOG_RED, LOG_ORANGE, LOG_GREEN, "", LOG_BLUE, LOG_CYBL}; /*!< \brief Optional start-format strings for highlighting */

static const char *const log_level_highlight_end[] =
    {LOG_RESET, LOG_RESET, LOG_RESET, LOG_RESET, LOG_RESET, LOG_RESET}; /*!< \brief Optional end-format strings for highlighting */
static void log_output_memory(log_component_t *c, const char *file, const char *func, int line, int comp, int level, const char* format,va_list args);

int write_file_matlab(const char *fname, const char *vname, void *data, int length, int dec, unsigned int format, int multiVec)
{
  FILE *fp=NULL;
  int i;

  AssertFatal((format&~MATLAB_RAW) <16,"");

  if (data == NULL)
    return -1;

  //printf("Writing %d elements of type %d to %s\n",length,format,fname);

  const int fname_len = strlen(fname);
  char write_file_name[fname_len + 10];
  const char *fn_format = (format & MATLAB_RAW) ? "%s.bin" : "%s";
  snprintf(write_file_name, sizeof(write_file_name), fn_format, fname);

  if (format == 10 || format ==11 || format == 12 || format == 13 || format == 14 || multiVec) {
    fp = fopen(write_file_name, "a+");
  } else if (format != 10 && format !=11  && format != 12 && format != 13 && format != 14) {
    fp = fopen(write_file_name, "w+");
  }

  if (fp== NULL) {
    printf("[OPENAIR][FILE OUTPUT] Cannot open file %s\n", write_file_name);
    return(-1);
  }

  if ( (format&MATLAB_RAW) == MATLAB_RAW ) {
    int sz[16]={sizeof(short), 2*sizeof(short),
		sizeof(int), 2*sizeof(int),
		sizeof(char), 2*sizeof(char),
		sizeof(long long), 
		sizeof(double), 2*sizeof(double),
		sizeof(unsigned char),
		sizeof(short),
		sizeof(short),
		sizeof(short),
		sizeof(short),
		sizeof(short),
		sizeof(short)
    };
    int eltSz= sz[format&~MATLAB_RAW];
    if (dec==1) 
      fwrite(data, eltSz, length, fp);
    else 
      for (i=0; i<length; i+=dec)
	fwrite(data+i*eltSz, eltSz, 1, fp);
    
    fclose(fp);
    return(0);	
  }

  if ((format != 10 && format !=11  && format != 12 && format != 13 && format != 14) || multiVec)
    fprintf(fp,"%s = [",vname);

  switch (format) {
    case 0:   // real 16-bit
      for (i=0; i<length; i+=dec) {
        fprintf(fp,"%d\n",((short *)data)[i]);
      }

      break;

    case 1:  // complex 16-bit
    case 13:
    case 14:
    case 15:
      for (i=0; i<length<<1; i+=(2*dec)) {
        fprintf(fp,"%d + j*(%d)\n",((short *)data)[i],((short *)data)[i+1]);
      }

      break;

    case 2:  // real 32-bit
      for (i=0; i<length; i+=dec) {
        fprintf(fp,"%d\n",((int *)data)[i]);
      }

      break;

    case 3: // complex 32-bit
      for (i=0; i<length<<1; i+=(2*dec)) {
        fprintf(fp,"%d + j*(%d)\n",((int *)data)[i],((int *)data)[i+1]);
      }

      break;

    case 4: // real 8-bit
      for (i=0; i<length; i+=dec) {
        fprintf(fp,"%d\n",((char *)data)[i]);
      }

      break;

    case 5: // complex 8-bit
      for (i=0; i<length<<1; i+=(2*dec)) {
        fprintf(fp,"%d + j*(%d)\n",((char *)data)[i],((char *)data)[i+1]);
      }

      break;

    case 6:  // real 64-bit
      for (i=0; i<length; i+=dec) {
        fprintf(fp,"%lld\n",((long long *)data)[i]);
      }

      break;

    case 7: // real double
      for (i=0; i<length; i+=dec) {
        fprintf(fp,"%g\n",((double *)data)[i]);
      }

      break;

    case 8: // complex double
      for (i=0; i<length<<1; i+=2*dec) {
        fprintf(fp,"%g + j*(%g)\n",((double *)data)[i], ((double *)data)[i+1]);
      }

      break;

    case 9: // real unsigned 8-bit
      for (i=0; i<length; i+=dec) {
        fprintf(fp,"%d\n",((unsigned char *)data)[i]);
      }

      break;

    case 10 : // case eren 16 bit complex :
      for (i=0; i<length<<1; i+=(2*dec)) {
        if((i < 2*(length-1)) && (i > 0))
          fprintf(fp,"%d + j*(%d),",((short *)data)[i],((short *)data)[i+1]);
        else if (i == 2*(length-1))
          fprintf(fp,"%d + j*(%d);",((short *)data)[i],((short *)data)[i+1]);
        else if (i == 0)
          fprintf(fp,"\n%d + j*(%d),",((short *)data)[i],((short *)data)[i+1]);
      }

      break;

    case 11 : //case eren 16 bit real for channel magnitudes:
      for (i=0; i<length; i+=dec) {
        if((i <(length-1))&& (i > 0))
          fprintf(fp,"%d,",((short *)data)[i]);
        else if (i == (length-1))
          fprintf(fp,"%d;",((short *)data)[i]);
        else if (i == 0)
          fprintf(fp,"\n%d,",((short *)data)[i]);
      }

      printf("\n eren: length :%d",length);
      break;

    case 12 : // case eren for log2_maxh real unsigned 8 bit
      fprintf(fp,"%d \n",((unsigned char *)&data)[0]);
      break;
  default:
    AssertFatal(false, "unknown dump format: %u\n", format);
  }

  if ((format != 10 && format !=11 && format !=12 && format != 13 && format != 15) || multiVec) {
    fprintf(fp,"];\n");
    fclose(fp);
    return(0);
  } else if (format == 10 || format ==11 || format == 12 || format == 13 || format == 15) {
    fclose(fp);
    return(0);
  }

  return 0;
}

#define FLAG_SETDEBUG(flag) g_log->debug_mask.DEBUG_##flag = *logparams_debug[i++].uptr;
#define FLAG_SETDUMP(flag) g_log->dump_mask.DEBUG_##flag = *logparams_dump[i++].uptr;

/* get log parameters from configuration file */
void log_getconfig(log_t *g_log)
{
  char *gloglevel = NULL;
  int consolelog = 0;
  paramdef_t logparams_defaults[] = LOG_GLOBALPARAMS_DESC;
  paramdef_t logparams_level[MAX_LOG_PREDEF_COMPONENTS];
  paramdef_t logparams_logfile[MAX_LOG_PREDEF_COMPONENTS];
  int ret = config_get(config_get_if(), logparams_defaults, sizeofArray(logparams_defaults), CONFIG_STRING_LOG_PREFIX);

  if (ret <0) {
    fprintf(stderr,"[LOG] init aborted, configuration couldn't be performed\n");
    return;
  }

  /* set LOG display options (enable/disable color, thread name, level ) */
  for(int i=0; i<logparams_defaults[LOG_OPTIONS_IDX].numelt ; i++) {
    for(int j=0; log_options[j].name != NULL ; j++) {
      if (strcmp(logparams_defaults[LOG_OPTIONS_IDX].strlistptr[i],log_options[j].name) == 0) {
        g_log->flag = g_log->flag |  log_options[j].value;
        break;
      } else if (log_options[j+1].name == NULL) {
        fprintf(stderr,"Unknown log option: %s\n",logparams_defaults[LOG_OPTIONS_IDX].strlistptr[i]);
        exit(-1);
      }
    }
  }

  /* build the parameter array for setting per component log level and infile options */
  memset(logparams_level, 0, sizeof(logparams_level));
  memset(logparams_logfile, 0, sizeof(logparams_logfile));

  for (int i = 0; i < MAX_LOG_PREDEF_COMPONENTS; i++) {
    if(g_log->log_component[i].name == NULL) {
      g_log->log_component[i].name = malloc(17);
      sprintf((char *)g_log->log_component[i].name,"comp%i?",i);
      logparams_logfile[i].paramflags = PARAMFLAG_DONOTREAD;
      logparams_level[i].paramflags = PARAMFLAG_DONOTREAD;
    }

    sprintf(logparams_level[i].optname,    LOG_CONFIG_LEVEL_FORMAT,       g_log->log_component[i].name);
    sprintf(logparams_logfile[i].optname,  LOG_CONFIG_LOGFILE_FORMAT,     g_log->log_component[i].name);

    /* workaround: all log options in existing configuration files use lower case component names
       where component names include uppercase char in log.h....                                */
    for (int j=0 ; j<strlen(logparams_level[i].optname); j++)
      logparams_level[i].optname[j] = tolower(logparams_level[i].optname[j]);

    for (int j=0 ; j<strlen(logparams_level[i].optname); j++)
      logparams_logfile[i].optname[j] = tolower(logparams_logfile[i].optname[j]);

    /* */
    logparams_level[i].defstrval     = gloglevel;
    logparams_logfile[i].defuintval  = 0;
    logparams_logfile[i].numelt      = 0;
    logparams_level[i].numelt        = 0;
    logparams_level[i].type          = TYPE_STRING;
    logparams_logfile[i].type        = TYPE_UINT;
    logparams_logfile[i].paramflags  = logparams_logfile[i].paramflags|PARAMFLAG_BOOL;
  }

  /* read the per component parameters */
  config_get(config_get_if(), logparams_level, MAX_LOG_PREDEF_COMPONENTS, CONFIG_STRING_LOG_PREFIX);
  config_get(config_get_if(), logparams_logfile, MAX_LOG_PREDEF_COMPONENTS, CONFIG_STRING_LOG_PREFIX);

  /* now set the log levels and infile option, according to what we read */
  for (int i = 0; i < MAX_LOG_PREDEF_COMPONENTS; i++) {
    g_log->log_component[i].level = map_str_to_int(log_level_names, *logparams_level[i].strptr);
    set_log(i, g_log->log_component[i].level);

    if (*logparams_logfile[i].uptr == 1)
      set_component_filelog(i);
  }

  /* build then read the debug and dump parameter array */
  int sz = 0;
  for (const char *const *ptr = flag_name; strlen(*ptr) > 1; ptr++)
    sz++;
  paramdef_t logparams_debug[sz];
  paramdef_t logparams_dump[sz];
  for (int i = 0; i < sz; i++) {
    logparams_debug[i] = (paramdef_t){
        .type = TYPE_UINT,
        .paramflags = PARAMFLAG_BOOL,
    };
    sprintf(logparams_debug[i].optname, LOG_CONFIG_DEBUG_FORMAT, flag_name[i]);
    logparams_dump[i] = (paramdef_t){.type = TYPE_UINT, .paramflags = PARAMFLAG_BOOL};
    sprintf(logparams_dump[i].optname, LOG_CONFIG_DUMP_FORMAT, flag_name[i]);
  }

  config_get(config_get_if(), logparams_debug, sz, CONFIG_STRING_LOG_PREFIX);
  config_get(config_get_if(), logparams_dump, sz, CONFIG_STRING_LOG_PREFIX);

  bool old = CONFIG_ISFLAGSET(CONFIG_NOABORTONCHKF);
  CONFIG_SETRTFLAG(CONFIG_NOABORTONCHKF);
  if (config_check_unknown_cmdlineopt(config_get_if(), CONFIG_STRING_LOG_PREFIX) > 0) {
    printf("Existing log_config options:\n");
    printf("   Boolean options:\n");
    for (int i = 0; i < sz; i++)
      printf("      %s, \t%s\n", logparams_debug[i].optname, logparams_dump[i].optname);
    printf("   Log level per module (");
    for (int i = 0; log_level_names[i].name != NULL; i++)
      printf("%s ", log_level_names[i].name);
    printf(")\n");
    for (int i = 0; i < MAX_LOG_PREDEF_COMPONENTS; i++)
      printf("      %s\n", logparams_level[i].optname);
    exit(1);
  }
  if (!old)
    CONFIG_CLEARRTFLAG(CONFIG_NOABORTONCHKF);
  int i = 0;
  FOREACH_FLAG(FLAG_SETDEBUG);
  i = 0;
  FOREACH_FLAG(FLAG_SETDUMP);

  /* log globally enabled/disabled */
  set_glog_onlinelog(consolelog);
}

int register_log_component(const char *name, const char *fext, int compidx)
{
  int computed_compidx=compidx;

  if (compidx < 0) { /* this is not a pre-defined component */
    for (int i = MAX_LOG_PREDEF_COMPONENTS; i< MAX_LOG_COMPONENTS; i++) {
      if (g_log->log_component[i].name == NULL) {
        computed_compidx=i;
        break;
      }
    }
  }

  if (computed_compidx >= 0 && computed_compidx <MAX_LOG_COMPONENTS) {
    g_log->log_component[computed_compidx].name = strdup(name);
    g_log->log_component[computed_compidx].stream = stdout;
    g_log->log_component[computed_compidx].filelog = 0;
    g_log->log_rarely_used[computed_compidx].filelog_name = calloc(1, strlen(name) + 16); /* /tmp/<name>.%s  */
    sprintf(g_log->log_rarely_used[computed_compidx].filelog_name, "/tmp/%s.", name);
    strncat(g_log->log_rarely_used[computed_compidx].filelog_name, fext, 3);
  } else {
    fprintf(stderr,"{LOG} %s %d Couldn't register component %s\n",__FILE__,__LINE__,name);
  }

  return computed_compidx;
}

static void unregister_all_log_components(void)
{
  log_component_t *lc = g_log->log_component;
  log_component_back_t *lb = g_log->log_rarely_used;
  while (lc->name) {
    free((char *)lc->name); // defined as const, but assigned through strdup()
    free(lb->filelog_name);
    lc++;
    lb++;
  }
}

int isLogInitDone (void)
{
  if (g_log == NULL)
    return 0;

  if (!(g_log->flag & FLAG_INITIALIZED))
    return 0;

  return 1;
}

int logInit (void)
{
  g_log = calloc(1, sizeof(log_t));

  if (g_log == NULL) {
    perror ("cannot allocated memory for log generation module \n");
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < MAX_LOG_PREDEF_COMPONENTS; i++)
    register_log_component(comp_name[i], comp_extension[i], i);

  for (int i=0 ; log_level_names[i].name != NULL ; i++)
    g_log->level2string[i] = toupper(log_level_names[i].name[0]); // uppercased first letter of level name
  
  g_log->filelog_name = "/tmp/openair.log";
  log_getconfig(g_log);

  // set all unused component items to 0, they are for non predefined components
  for (int i=MAX_LOG_PREDEF_COMPONENTS; i < MAX_LOG_COMPONENTS; i++) {
    memset(&(g_log->log_component[i]),0,sizeof(log_component_t));
  }

  AssertFatal(__builtin_popcount(g_log->flag & (FLAG_TIME | FLAG_REAL_TIME | FLAG_UTC_TIME)) <= 1,
          "Invalid log options: time, wall_clock and utc_time are mutually exclusive\n");

  g_log->flag =  g_log->flag | FLAG_INITIALIZED;
  return 0;
}

void logTerm(void)
{
  unregister_all_log_components();
  free_and_zero(g_log);
}

#include <sys/syscall.h>
static inline int log_header(log_component_t *c,
			     char *log_buffer,
			     int buffsize,
			     const char *file,
			     const char *func,
			     int line,
			     int level)
{
  int flag = g_log->flag;

  char threadname[64];
  if (flag & FLAG_THREAD ) {
    threadname[0]='{';
    if (pthread_getname_np(pthread_self(), threadname + 1, sizeof(threadname) - 3) != 0)
      strcpy(threadname+1, "?thread?");
    strcat(threadname,"} ");
  } else {
    threadname[0]=0;
  }

  char l[32];
  if (flag & FLAG_FILE_LINE && flag & FLAG_FUNCT )
    snprintf(l, sizeof l, "(%.23s:%d) ", func, line);
  else if (flag & FLAG_FILE_LINE)
    snprintf(l, sizeof l, "(%d) ", line);
  else if (flag & FLAG_FUNCT)
    snprintf(l, sizeof l, "(%.28s) ", func);
  else
    l[0] = 0;

  // output time information
  char timeString[64];
  if ((flag & FLAG_TIME) || (flag & FLAG_REAL_TIME) || (flag & FLAG_UTC_TIME)) {
    struct timespec t;
    const clockid_t clock = flag & FLAG_TIME ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    if (clock_gettime(clock, &t) == -1)
       abort();
    if (flag & FLAG_UTC_TIME) {
      struct tm utc_time;
      if (gmtime_r(&t.tv_sec, &utc_time) == NULL)
        abort();
      snprintf(timeString, sizeof(timeString), "%04d-%02d-%02d %02d:%02d:%02d.%06lu UTC ",
               utc_time.tm_year + 1900, utc_time.tm_mon + 1, utc_time.tm_mday,
               utc_time.tm_hour, utc_time.tm_min, utc_time.tm_sec, t.tv_nsec / 1000);
    } else {
      snprintf(timeString, sizeof(timeString), "%lu.%06lu ",
               t.tv_sec,
               t.tv_nsec / 1000);
    }
  } else {
    timeString[0] = 0;
  }

  char threadIdString[32];
  if (flag & FLAG_THREAD_ID) {
    snprintf(threadIdString, sizeof(threadIdString), "%08lx ", syscall(__NR_gettid));
  } else {
    threadIdString[0] = 0;
  }
  return snprintf(log_buffer, buffsize, "%s%s%s[%s] %c %s%s",
		   flag & FLAG_NOCOLOR ? "" : log_level_highlight_start[level],
		   timeString,
		   threadIdString,
		   c->name,
		   flag & FLAG_LEVEL ? g_log->level2string[level] : ' ',
		   l,
		   threadname
		   );
}

void logRecord_mt(const char *file,
		  const char *func,
		  int line,
		  int comp,
		  int level,
		  const char *format,
		  ... )
{
  log_component_t *c = &g_log->log_component[comp];
  va_list args;
  va_start(args,format);
  log_output_memory(c, file,func,line,comp,level,format,args);
  va_end(args);
}
#if ENABLE_LTTNG
void logRecord_lttng(const char *file, const char *func, int line, int comp, int level, const char *format, ...)
{
  log_component_t *c = &g_log->log_component[comp];
  char header[48];
  char buf[MAX_LOG_TOTAL];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf) - 1, format, args);
  va_end(args);

  if (map_int_to_str(log_level_names, level) != NULL)
    snprintf(header, sizeof(header), "OAI-%s %s", c->name, map_int_to_str(log_level_names, level));
  else
    snprintf(header, sizeof(header), "OAI-%s", c->name);

  LOG_FC(header, func, line, buf);
}
#endif

void vlogRecord_mt(const char *file,
		   const char *func,
		   int line,
		   int comp,
		   int level,
		   const char *format,
		   va_list args )
{
  log_component_t *c = &g_log->log_component[comp];
  log_output_memory(c, file,func,line,comp,level, format,args);
}

void log_dump(int component,
	      void *buffer,
	      int buffsize,
	      int datatype,
	      const char *format,
	      ... )
{
  va_list args;
  char *wbuf;
  log_component_t *c = &g_log->log_component[component];
  int flag = g_log->flag;

  switch(datatype) {
    case LOG_DUMP_DOUBLE:
      wbuf=malloc((buffsize * 10)  + 64 + MAX_LOG_TOTAL);
      break;

    case LOG_DUMP_C16:
      wbuf = malloc((buffsize * 10) + 64 + MAX_LOG_TOTAL);
      break;

    case LOG_DUMP_CHAR:
    default:
      wbuf=malloc((buffsize * 3 ) + 64 + MAX_LOG_TOTAL);
      break;
  }

  if (wbuf != NULL) {
    va_start(args, format);
    int pos=log_header(c, wbuf,MAX_LOG_TOTAL,"noFile","noFunc",0, OAILOG_INFO);
    pos+=vsprintf(wbuf+pos,format, args);
    va_end(args);

    for (int i=0; i<buffsize; i++) {
      switch(datatype) {
        case LOG_DUMP_DOUBLE:
          pos = pos + sprintf(wbuf+pos,"%04.4lf ", (double)((double *)buffer)[i]);
          break;

        case LOG_DUMP_I16: {
          int16_t *tmp = ((int16_t *)buffer) + i;
          pos = pos + sprintf(wbuf + pos, "%d, ", *tmp);
        } break;

        case LOG_DUMP_C16: {
          int16_t *tmp = ((int16_t *)buffer) + i * 2;
          pos = pos + sprintf(wbuf + pos, "(%d,%d), ", *tmp, *(tmp + 1));
        } break;

        case LOG_DUMP_C32: {
          int32_t *tmp = ((int32_t *)buffer) + i * 2;
          pos = pos + sprintf(wbuf + pos, "(%d,%d), ", *tmp, *(tmp + 1));
        } break;

        case LOG_DUMP_CHAR:
        default:
          pos = pos + sprintf(wbuf+pos,"%02x ", (unsigned char)((unsigned char *)buffer)[i]);
          break;
      }
    }
    if ( flag & FLAG_NOCOLOR )
      sprintf(wbuf+pos,"\n");
    else
      sprintf(wbuf+pos,"%s\n",log_level_highlight_end[OAILOG_INFO]);
    c->print(c->stream,wbuf);
    free(wbuf);
  }
}

int set_log(int component,
		    int level)
{
  /* Checking parameters */
  DevCheck((component >= 0) && (component < MAX_LOG_COMPONENTS), component, 0, MAX_LOG_COMPONENTS);
  DevCheck((level < NUM_LOG_LEVEL) && (level >= OAILOG_DISABLE), level, NUM_LOG_LEVEL,
           OAILOG_ERR);

  if ( g_log->log_component[component].level != OAILOG_DISABLE )
    g_log->log_rarely_used[component].savedlevel = g_log->log_component[component].level;

  g_log->log_component[component].level = level;
  return 0;
}

void set_glog(int level)
{
  for (int c=0; c< MAX_LOG_COMPONENTS; c++ ) {
    set_log(c, level);
  }
}

void set_glog_onlinelog(int enable)
{
  for (int c=0; c< MAX_LOG_COMPONENTS; c++ ) {
    if ( enable ) {
      g_log->log_component[c].level = g_log->log_rarely_used[c].savedlevel;
      g_log->log_component[c].vprint = vfprintf;
      g_log->log_component[c].print = fprintf;
      g_log->log_component[c].stream = stdout;
    } else {
      g_log->log_component[c].level = OAILOG_DISABLE;
    }
  }
}

void set_glog_filelog(int enable)
{
  static FILE *fptr;

  if ( enable ) {
    fptr = fopen(g_log->filelog_name,"w");

    for (int c=0; c< MAX_LOG_COMPONENTS; c++ ) {
      close_component_filelog(c);
      g_log->log_component[c].stream = fptr;
      g_log->log_component[c].filelog =  1;
    }
  } else {
    for (int c=0; c< MAX_LOG_COMPONENTS; c++ ) {
      g_log->log_component[c].filelog =  0;

      if (fptr != NULL) {
        fclose(fptr);
      }

      g_log->log_component[c].stream = stdout;
    }
  }
}

void set_component_filelog(int comp)
{
  if (g_log->log_component[comp].stream == NULL || g_log->log_component[comp].stream == stdout) {
    g_log->log_component[comp].stream = fopen(g_log->log_rarely_used[comp].filelog_name, "w");
  }

  g_log->log_component[comp].vprint = vfprintf;
  g_log->log_component[comp].print = fprintf;
  g_log->log_component[comp].filelog =  1;
}

void close_component_filelog(int comp)
{
  g_log->log_component[comp].filelog =  0;

  if (g_log->log_component[comp].stream != NULL && g_log->log_component[comp].stream != stdout ) {
    fclose(g_log->log_component[comp].stream);
    g_log->log_component[comp].stream = stdout;
  }

  g_log->log_component[comp].vprint = vfprintf;
  g_log->log_component[comp].print = fprintf;
}

/*
 * for the two functions below, the passed array must have a final entry
 * with string value NULL
 */
/* map a string to an int. Takes a mapping array and a string as arg */
int map_str_to_int(const mapping *map, const char *str)
{
  while (1) {
    if (map->name == NULL) {
      return(-1);
    }

    if (!strcmp(map->name, str)) {
      return(map->value);
    }

    map++;
  }
}

/* map an int to a string. Takes a mapping array and a value */
char *map_int_to_str(const mapping *map, const int val)
{
  while (map->name) {
    if (map->value == val) {
      return map->name;
    }
    map++;
  }
  return NULL;
}

void logClean (void)
{
  if(isLogInitDone()) {
    for (int i = 0; i < MAX_LOG_COMPONENTS; i++) {
      close_component_filelog(i);
    }
  }
}

static atomic_bool stop_flush_mem_to_file = false;
void flush_mem_to_file(void)
{
  int fp;
  char f_name[1024];
  struct timespec slp_tm;
  slp_tm.tv_sec = 0;
  slp_tm.tv_nsec = 10000;
  
  pthread_setname_np( pthread_self(), "flush_mem_to_file");

  while (!atomic_load(&stop_flush_mem_to_file)) {
    pthread_mutex_lock(&log_mem_lock);
    log_mem_write_flag=0;
    pthread_cond_wait(&log_mem_notify, &log_mem_lock);
    log_mem_write_flag=1;
    pthread_mutex_unlock(&log_mem_lock);
    // write!
    if(log_mem_d[log_mem_write_side].enable_flag==0){
      if(log_mem_file_cnt>5){
        log_mem_file_cnt=5;
        printf("log over write!!!\n");
      }
      snprintf(f_name,1024, "%s_%d.log",log_mem_filename,log_mem_file_cnt);
      fp=open(f_name, O_WRONLY | O_CREAT, 0666);
      int ret = write(fp, log_mem_d[log_mem_write_side].buf_p, log_mem_d[log_mem_write_side].buf_index);
      if ( ret < 0) {
          fprintf(stderr,"{LOG} %s %d Couldn't write in %s \n",__FILE__,__LINE__,f_name);
          exit(EXIT_FAILURE);
      }
      close(fp);
      log_mem_file_cnt++;
      log_mem_d[log_mem_write_side].buf_index=0;
      log_mem_d[log_mem_write_side].enable_flag=1;
    }else{
      printf("If you'd like to write log, you should set enable flag to 0!!!\n");
      nanosleep(&slp_tm,NULL);
    }
  }
}

static void log_output_memory(log_component_t *c, const char *file, const char *func, int line, int comp, int level, const char* format,va_list args)
{
  //logRecord_mt(file,func,line, pthread_self(), comp, level, format, ##args)
  int len = 0;
  /* The main difference with the version above is the use of this local log_buffer.
   * The other difference is the return value of snprintf which was not used
   * correctly. It was not a big problem because in practice MAX_LOG_TOTAL is
   * big enough so that the buffer is never full.
   */
  static_assert(4 * MAX_LOG_TOTAL <= 65536, "log buffer limited to 64kB, please reduce MAX_LOG_TOTAL\n");
  char log_buffer[4 * MAX_LOG_TOTAL];

  // make sure that for log trace the extra info is only printed once, reset when the level changes
  if (level < OAILOG_TRACE) {
    int n = log_header(c, log_buffer+len, sizeof(log_buffer), file, func, line, level);
    if (n > 0) {
      len += n;
      if (len > sizeof(log_buffer)) {
        len = sizeof(log_buffer);
      }
    }
  }
  int n = vsnprintf(log_buffer+len, sizeof(log_buffer)-len, format, args);
  if (n > 0) {
    len += n;
    if (len > sizeof(log_buffer)) {
      len = sizeof(log_buffer);
    }
  }
  if (!((g_log->flag) & FLAG_NOCOLOR)) {
    int n = snprintf(log_buffer+len, sizeof(log_buffer)-len, "%s", log_level_highlight_end[level]);
    if (n > 0) {
      len += n;
      if (len > sizeof(log_buffer)) {
        len = sizeof(log_buffer);
      }
    }
  }

  // OAI printf compatibility
  if(log_mem_flag==1){
    if(log_mem_d[log_mem_side].enable_flag==1){
      int temp_index;
        temp_index=log_mem_d[log_mem_side].buf_index;
        if(temp_index+len+1 < LOG_MEM_SIZE){
          log_mem_d[log_mem_side].buf_index+=len;
          memcpy(&log_mem_d[log_mem_side].buf_p[temp_index],log_buffer,len);
        }else{
          log_mem_d[log_mem_side].enable_flag=0;
          if(log_mem_d[1-log_mem_side].enable_flag==1){
            temp_index=log_mem_d[1-log_mem_side].buf_index;
            if(temp_index+len+1 < LOG_MEM_SIZE){
              log_mem_d[1-log_mem_side].buf_index+=len;
              log_mem_side=1-log_mem_side;
              memcpy(&log_mem_d[log_mem_side].buf_p[temp_index],log_buffer,len);
              /* write down !*/
              if (pthread_mutex_lock(&log_mem_lock) != 0) {
                return;
              }
              if(log_mem_write_flag==0){
                log_mem_write_side=1-log_mem_side;
                if(pthread_cond_signal(&log_mem_notify) != 0) {
                }
              }
              if(pthread_mutex_unlock(&log_mem_lock) != 0) {
                return;
              }
            }else{
              log_mem_d[1-log_mem_side].enable_flag=0;
            }
          }
        }
      }
  }else{
    AssertFatal(len >= 0 && len <= sizeof(log_buffer), "Bad len %d\n", len);
    if (write(fileno(c->stream), log_buffer, len)) {};
  }
}

int logInit_log_mem (char * filename)
{
  log_mem_flag = 1;
  log_mem_filename = filename; // in present code, the parameter is safe permanent pointer
  for (log_mem_cnt_t *ptr = log_mem_d; ptr < log_mem_d + sizeofArray(log_mem_d); ptr++)
    *ptr = (log_mem_cnt_t){
        .buf_p = malloc(LOG_MEM_SIZE),
        .buf_index = 0,
        .enable_flag = 1,
    };
  log_mem_side = 0;
  if ((pthread_mutex_init(&log_mem_lock, NULL) != 0) || (pthread_cond_init(&log_mem_notify, NULL) != 0)) {
    log_mem_d[1].enable_flag=0;
    return -1;
  }
  pthread_create(&log_mem_thread, NULL, (void *(*)(void *))flush_mem_to_file, (void *)NULL);
  
  return 0;
}

void close_log_mem(void){
  int fp;
  char f_name[1024];

  if(log_mem_flag==1){
    atomic_store(&stop_flush_mem_to_file, false);
    log_mem_d[0].enable_flag=0;
    log_mem_d[1].enable_flag=0;
    usleep(10); // wait for log writing
    while(log_mem_write_flag==1){
      usleep(100);
    }
      snprintf(f_name,1024, "%s_%d.log",log_mem_filename,log_mem_file_cnt);
      fp=open(f_name, O_WRONLY | O_CREAT, 0666);
      int ret = write(fp, log_mem_d[0].buf_p, log_mem_d[0].buf_index);
      if ( ret < 0) {
          fprintf(stderr,"{LOG} %s %d Couldn't write in %s \n",__FILE__,__LINE__,f_name);
          exit(EXIT_FAILURE);
      }
      close(fp);
      free(log_mem_d[0].buf_p);
      
      snprintf(f_name,1024, "%s_%d.log",log_mem_filename,log_mem_file_cnt);
      fp=open(f_name, O_WRONLY | O_CREAT, 0666);
      ret = write(fp, log_mem_d[1].buf_p, log_mem_d[1].buf_index);
      if ( ret < 0) {
          fprintf(stderr,"{LOG} %s %d Couldn't write in %s \n",__FILE__,__LINE__,f_name);
          exit(EXIT_FAILURE);
      }
      close(fp);
      free(log_mem_d[1].buf_p);
  }
 }

#ifdef LOG_TEST

int main(int argc, char *argv[]) {
  logInit();
  test_log();
  return 1;
}

int test_log(void) {
  LOG_ENTER(MAC); // because the default level is DEBUG
  LOG_I(EMU, "1 Starting OAI logs version %s Build date: %s on %s\n",
        BUILD_VERSION, BUILD_DATE, BUILD_HOST);
  LOG_D(MAC, "1 debug  MAC \n");
  LOG_W(MAC, "1 warning MAC \n");
  set_log(EMU, OAILOG_INFO);
  set_log(MAC, OAILOG_WARNING);
  LOG_I(EMU, "2 Starting OAI logs version %s Build date: %s on %s\n",
        BUILD_VERSION, BUILD_DATE, BUILD_HOST);
  LOG_E(MAC, "2 error MAC\n");
  LOG_D(MAC, "2 debug  MAC \n");
  LOG_W(MAC, "2 warning MAC \n");
  LOG_I(MAC, "2 info MAC \n");
  set_log(MAC, OAILOG_NOTICE);
  LOG_ENTER(MAC);
  LOG_I(EMU, "3 Starting OAI logs version %s Build date: %s on %s\n",
        BUILD_VERSION, BUILD_DATE, BUILD_HOST);
  LOG_D(MAC, "3 debug  MAC \n");
  LOG_W(MAC, "3 warning MAC \n");
  LOG_I(MAC, "3 info MAC \n");
  set_log(MAC, LOG_DEBUG);
  set_log(EMU, LOG_DEBUG);
  LOG_ENTER(MAC);
  LOG_I(EMU, "4 Starting OAI logs version %s Build date: %s on %s\n",
        BUILD_VERSION, BUILD_DATE, BUILD_HOST);
  LOG_D(MAC, "4 debug  MAC \n");
  LOG_W(MAC, "4 warning MAC \n");
  LOG_I(MAC, "4 info MAC \n");
  set_log(MAC, LOG_DEBUG);
  set_log(EMU, LOG_DEBUG);
  LOG_I(LOG, "5 Starting OAI logs version %s Build date: %s on %s\n",
        BUILD_VERSION, BUILD_DATE, BUILD_HOST);
  LOG_D(MAC, "5 debug  MAC \n");
  LOG_W(MAC, "5 warning MAC \n");
  LOG_I(MAC, "5 info MAC \n");
  set_log(MAC, LOG_TRACE);
  set_log(EMU, LOG_TRACE);
  LOG_ENTER(MAC);
  LOG_I(LOG, "6 Starting OAI logs version %s Build date: %s on %s\n",
        BUILD_VERSION, BUILD_DATE, BUILD_HOST);
  LOG_D(MAC, "6 debug  MAC \n");
  LOG_W(MAC, "6 warning MAC \n");
  LOG_I(MAC, "6 info MAC \n");
  LOG_EXIT(MAC);
  return 0;
}
#endif
