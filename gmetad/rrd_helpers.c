/* $Id$ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <rrd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "gmetad.h"

#define PATHSIZE 4096
extern gmetad_config_t gmetad_config;


pthread_mutex_t rrd_mutex = PTHREAD_MUTEX_INITIALIZER;

static void inline
my_mkdir ( const char *dir )
{
   if ( mkdir ( dir, 0755 ) < 0 && errno != EEXIST)
      {
         err_sys("Unable to mkdir(%s)",dir);
      }
}

static int
RRD_update( char *rrd, const char *sum, const char *num, unsigned int process_time )
{
   char *argv[3];
   int   argc = 3;
   char val[128];

   /* If we are a host RRD, we "sum" over only one host. */
   if (num)
      sprintf(val, "%d:%s:%s", process_time, sum, num);
   else
      sprintf(val, "%d:%s", process_time, sum);

   argv[0] = "dummy";
   argv[1] = rrd;
   argv[2] = val; 

   pthread_mutex_lock( &rrd_mutex );
   optind=0; opterr=0;
   rrd_clear_error();
   rrd_update(argc, argv);
   if(rrd_test_error())
      {
         err_msg("RRD_update (%s): %s", rrd, rrd_get_error());
         pthread_mutex_unlock( &rrd_mutex );
         return 1;
      } 
   /* debug_msg("Updated rrd %s with value %s", rrd, val); */
   pthread_mutex_unlock( &rrd_mutex );
   return 0;
}


/* Warning: RRD_create will overwrite a RRdb if it already exists */
static int
RRD_create( char *rrd, int summary, unsigned int process_time)
{
#define MAX_CREATE_ARGS 64
   char *argv[MAX_CREATE_ARGS];
   int  argc=0, i;
   char start[64];
   char sum[64];
   char num[64];
   int heartbeat = 315360000; /* 1 decade */

   argv[argc++] = "dummy";
   argv[argc++] = rrd;
   argv[argc++] = "--step";
   argv[argc++] = "1";
   argv[argc++] = "--start";
   sprintf(start, "%u", process_time-1);
   argv[argc++] = start;
   sprintf(sum,"DS:sum:GAUGE:%d:U:U", heartbeat);
   argv[argc++] = sum;
   if (summary) {
      sprintf(num,"DS:num:GAUGE:%d:U:U", heartbeat);
      argv[argc++] = num;
   }

   for(i = 0; i< gmetad_config.num_rras; i++)
     {
       argv[argc++] = gmetad_config.rras[i];
       if(argc>=MAX_CREATE_ARGS)
	 {
	   fprintf(stderr,"Too many round-robin archives.  Check config file.\n");
	   exit(1);
	 }
     }

   /* This is the pre-2.6.0 format
   argv[argc++] = "RRA:AVERAGE:0.5:1:240";
   argv[argc++] = "RRA:AVERAGE:0.5:24:240";
   argv[argc++] = "RRA:AVERAGE:0.5:168:240";
   argv[argc++] = "RRA:AVERAGE:0.5:672:240";
   argv[argc++] = "RRA:AVERAGE:0.5:5760:370";
   */

   pthread_mutex_lock( &rrd_mutex );
   optind=0; opterr=0;
   rrd_clear_error();
   rrd_create(argc, argv);
   if(rrd_test_error())
      {
         err_msg("RRD_create: %s", rrd_get_error());
         pthread_mutex_unlock( &rrd_mutex );
         return 1;
      }
   debug_msg("Created rrd %s", rrd);
   pthread_mutex_unlock( &rrd_mutex );
   return 0;
}


/* A summary RRD has a "num" and a "sum" DS (datasource) whereas the
   host rrds only have "sum" (since num is always 1) */
static int
push_data_to_rrd( char *rrd, const char *sum, const char *num, unsigned int process_time)
{
   int rval;
   int summary;
   struct stat st;

   /*  if process_time is undefined, we set it to the current time */
   if (!process_time)
      process_time = time(0);

   if (num)
      summary=1;
   else
      summary=0;

   if( stat(rrd, &st) )
      {
         rval = RRD_create( rrd, summary, process_time );
         if( rval )
            return rval;
      }
   return RRD_update( rrd, sum, num, process_time );
}

/* Returns the last position in the string (not \0) */
char *
lowercase_it( char *string )
{
  char *p;
   /* We need to make the file path all lower-case since most
      good filesystems are case-sensitive.  We don't want
      host00.foo.bar and host00.Foo.Bar data to be save to two
      different locations */
  for (p = string; p && *p; p++)
     {
       *p = tolower (*p);
     }

  return p;
}


/* Assumes num argument will be NULL for a host RRD. */
int
write_data_to_rrd ( const char *source, const char *host, const char *metric, 
   const char *sum, const char *num, unsigned int step, unsigned int process_time )
{
   char rrd[ PATHSIZE ];
   char *summary_dir = "__summaryinfo__";
   char *p;

   /* Build the path to our desired RRD file. Assume the rootdir exists. */
   strcpy(rrd, gmetad_config.rrd_rootdir);
   p = lowercase_it( rrd );

   if (source) {
      strncat(rrd, "/", PATHSIZE);
      strncat(rrd, source, PATHSIZE);
      p = lowercase_it( p );
      my_mkdir( rrd );
   }

   if (host) {
      strncat(rrd, "/", PATHSIZE);
      strncat(rrd, host, PATHSIZE);
      p = lowercase_it( p );
      my_mkdir( rrd );
   }
   else {
      strncat(rrd, "/", PATHSIZE);
      strncat(rrd, summary_dir, PATHSIZE);
      p = lowercase_it( p );
      my_mkdir( rrd );
   }

   strncat(rrd, "/", PATHSIZE);
   strncat(rrd, metric, PATHSIZE);
   strncat(rrd, ".rrd", PATHSIZE);
   p = lowercase_it ( p );

   return push_data_to_rrd( rrd, sum, num, process_time );
}
