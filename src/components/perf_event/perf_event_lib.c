/*
* File:    perf_events.c
*
* Author:  Corey Ashford
*          cjashfor@us.ibm.com
*          - based upon perfmon.c written by -
*          Philip Mucci
*          mucci@cs.utk.edu
* Mods:    Gary Mohr
*          gary.mohr@bull.com
* Mods:    Vince Weaver
*          vweaver1@eecs.utk.edu
* Mods:	   Philip Mucci
*	   mucci@eecs.utk.edu */


#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <syscall.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

/* PAPI-specific includes */
#include "papi.h"
#include "papi_memory.h"
#include "papi_internal.h"
#include "papi_vector.h"
#include "extras.h"

/* libpfm4 includes */
#include "papi_libpfm4_events.h"
#include "perfmon/pfmlib.h"
#include PEINCLUDE

/* Linux-specific includes */
#include "mb.h"
#include "linux-memory.h"
#include "linux-timer.h"
#include "linux-common.h"
#include "linux-context.h"

#include "perf_event_lib.h"

/* Defines for ctx->state */
#define PERF_EVENTS_OPENED  0x01
#define PERF_EVENTS_RUNNING 0x02

/* Static globals */
int nmi_watchdog_active;


/******** Kernel Version Dependent Routines  **********************/

/* KERNEL_CHECKS_SCHEDUABILITY_UPON_OPEN is a work-around for kernel arch
 * implementations (e.g. x86) which don't do a static event scheduability
 * check in sys_perf_event_open.  
 * This was fixed for x86 in the 2.6.33 kernel
 *
 * Also! Kernels newer than 2.6.34 will fail in a similar way
 *       if the nmi_watchdog has stolen a performance counter
 *       and we try to use the maximum number of counters.
 *       A sys_perf_event_open() will seem to succeed but will fail
 *       at read time.  So re-use this work around code.
 */
static int 
bug_check_scheduability(void) {

#if defined(__powerpc__)
  /* PowerPC not affected by this bug */
#elif defined(__mips__)
  /* MIPS as of kernel 3.1 does not properly detect schedulability */
  return 1;
#else
  if (_papi_os_info.os_version < LINUX_VERSION(2,6,33)) return 1;
#endif

  if (nmi_watchdog_active) return 1;

  return 0;
}

/* PERF_FORMAT_GROUP allows reading an entire group's counts at once   */
/* before 2.6.34 PERF_FORMAT_GROUP did not work when reading results   */
/*  from attached processes.  We are lazy and disable it for all cases */
/*  commit was:  050735b08ca8a016bbace4445fa025b88fee770b              */

static int 
bug_format_group(void) {

  if (_papi_os_info.os_version < LINUX_VERSION(2,6,34)) return 1;

  /* MIPS, as of version 3.1, does not support this properly */

#if defined(__mips__)
  return 1;
#endif

  return 0;

}


/* There's a bug prior to Linux 2.6.33 where if you are using */
/* PERF_FORMAT_GROUP, the TOTAL_TIME_ENABLED and              */
/* TOTAL_TIME_RUNNING fields will be zero unless you disable  */
/* the counters first                                         */
static int 
bug_sync_read(void) {

  if (_papi_os_info.os_version < LINUX_VERSION(2,6,33)) return 1;

  return 0;

}


/* Set the F_SETOWN_EX flag on the fd.                          */
/* This affects which thread an overflow signal gets sent to    */
/* Handled in a subroutine to handle the fact that the behavior */
/* is dependent on kernel version.                              */
static int 
fcntl_setown_fd(int fd) {

   int ret;
   struct f_owner_ex fown_ex;

      /* F_SETOWN_EX is not available until 2.6.32 */
   if (_papi_os_info.os_version < LINUX_VERSION(2,6,32)) {
	   
      /* get ownership of the descriptor */
      ret = fcntl( fd, F_SETOWN, mygettid(  ) );
      if ( ret == -1 ) {
	 PAPIERROR( "cannot fcntl(F_SETOWN) on %d: %s", fd, strerror(errno) );
	 return PAPI_ESYS;
      }
   }
   else {
      /* set ownership of the descriptor */   
      fown_ex.type = F_OWNER_TID;
      fown_ex.pid  = mygettid();
      ret = fcntl(fd, F_SETOWN_EX, (unsigned long)&fown_ex );
   
      if ( ret == -1 ) {
	 PAPIERROR( "cannot fcntl(F_SETOWN_EX) on %d: %s", 
		    fd, strerror( errno ) );
	 return PAPI_ESYS;
      }
   }
   return PAPI_OK;
}

/* The read format on perf_event varies based on various flags that */
/* are passed into it.  This helper avoids copying this logic       */
/* multiple places.                                                 */
static unsigned int
get_read_format( unsigned int multiplex, 
		 unsigned int inherit, 
		 int format_group )
{
   unsigned int format = 0;

   /* if we need read format options for multiplexing, add them now */
   if (multiplex) {
      format |= PERF_FORMAT_TOTAL_TIME_ENABLED;
      format |= PERF_FORMAT_TOTAL_TIME_RUNNING;
   }

   /* if our kernel supports it and we are not using inherit, */
   /* add the group read options                              */
   if ( (!bug_format_group()) && !inherit) {
      if (format_group) {
	 format |= PERF_FORMAT_GROUP;
      }
   }

   SUBDBG("multiplex: %d, inherit: %d, group_leader: %d, format: %#x\n",
	  multiplex, inherit, format_group, format);

   return format;
}

/********* End Kernel-version Dependent Routines  ****************/

static int map_perf_event_errors_to_papi(int perf_event_error) {

   int ret;

   /* These mappings are approximate.
      EINVAL in particular can mean lots of different things */
   switch(perf_event_error) {
      case EPERM:
      case EACCES:
           ret = PAPI_EPERM;
	   break;
      case ENODEV:
      case EOPNOTSUPP:
	   ret = PAPI_ENOSUPP;
           break;
      case ENOENT:
	   ret = PAPI_ENOEVNT;
           break;
      case ENOSYS:
      case EAGAIN:
      case EBUSY:
      case E2BIG:
	   ret = PAPI_ESYS;
	   break;
      case ENOMEM:
	   ret = PAPI_ENOMEM;
	   break;
      case EINVAL:
      default:
	   ret = PAPI_EINVAL;
           break;
   }
   return ret;
}


/** Check if the current set of options is supported by  */
/*  perf_events.                                         */
/*  We do this by temporarily opening an event with the  */
/*  desired options then closing it again.  We use the   */
/*  PERF_COUNT_HW_INSTRUCTION event as a dummy event     */
/*  on the assumption it is available on all             */
/*  platforms.                                           */

static int
check_permissions( unsigned long tid, 
		   unsigned int cpu_num, 
		   unsigned int domain, 
		   unsigned int granularity,
		   unsigned int multiplex, 
		   unsigned int inherit )
{
   int ev_fd;
   struct perf_event_attr attr;

   long pid;

   /* clearing this will set a type of hardware and to count all domains */
   memset(&attr, '\0', sizeof(attr));
   attr.read_format = get_read_format(multiplex, inherit, 1);

   /* set the event id (config field) to instructios */
   /* (an event that should always exist)            */
   /* This was cycles but that is missing on Niagara */
   attr.config = PERF_COUNT_HW_INSTRUCTIONS;
	
   /* now set up domains this event set will be counting */
   if (!(domain & PAPI_DOM_SUPERVISOR)) {
      attr.exclude_hv = 1;
   }
   if (!(domain & PAPI_DOM_USER)) {
      attr.exclude_user = 1;
   }
   if (!(domain & PAPI_DOM_KERNEL)) {
      attr.exclude_kernel = 1;
   }

   if (granularity==PAPI_GRN_SYS) {
      pid = -1;
   } else {
      pid = tid;
   }

   SUBDBG("Calling sys_perf_event_open() from check_permissions\n");

   ev_fd = sys_perf_event_open( &attr, pid, cpu_num, -1, 0 );
   if ( ev_fd == -1 ) {
      SUBDBG("sys_perf_event_open returned error.  Linux says, %s", 
	     strerror( errno ) );
      return map_perf_event_errors_to_papi(errno);
   }
	
   /* now close it, this was just to make sure we have permissions */
   /* to set these options                                         */
   close(ev_fd);
   return PAPI_OK;
}



/* Maximum size we ever expect to read from a perf_event fd   */
/*  (this is the number of 64-bit values)                     */
/* We use this to size the read buffers                       */
/* The three is for event count, time_enabled, time_running   */
/*  and the counter term is count value and count id for each */
/*  possible counter value.                                   */
#define READ_BUFFER_SIZE (3 + (2 * PERF_EVENT_MAX_MPX_COUNTERS))



/* KERNEL_CHECKS_SCHEDUABILITY_UPON_OPEN is a work-around for kernel arch */
/* implementations (e.g. x86 before 2.6.33) which don't do a static event */
/* scheduability check in sys_perf_event_open.  It is also needed if the  */
/* kernel is stealing an event, such as when NMI watchdog is enabled.     */

static int
check_scheduability( pe_context_t *ctx, pe_control_t *ctl, int idx )
{
   int retval = 0, cnt = -1;
   ( void ) ctx;			 /*unused */
   long long papi_pe_buffer[READ_BUFFER_SIZE];
   int i,group_leader_fd;

   if (bug_check_scheduability()) {

      /* If the kernel isn't tracking scheduability right       */
      /* Then we need to start/stop/read to force the event     */
      /* to be scheduled and see if an error condition happens. */

      /* get the proper fd to start */
      group_leader_fd=ctl->events[idx].group_leader_fd;
      if (group_leader_fd==-1) group_leader_fd=ctl->events[idx].event_fd;

      /* start the event */
      retval = ioctl( group_leader_fd, PERF_EVENT_IOC_ENABLE, NULL );
      if (retval == -1) {
	 PAPIERROR("ioctl(PERF_EVENT_IOC_ENABLE) failed.\n");
	 return PAPI_ESYS;
      }

      /* stop the event */
      retval = ioctl(group_leader_fd, PERF_EVENT_IOC_DISABLE, NULL );
      if (retval == -1) {
	 PAPIERROR( "ioctl(PERF_EVENT_IOC_DISABLE) failed.\n" );
	 return PAPI_ESYS;
      }

      /* See if a read returns any results */
      cnt = read( group_leader_fd, papi_pe_buffer, sizeof(papi_pe_buffer));
      if ( cnt == -1 ) {
	 SUBDBG( "read returned an error!  Should never happen.\n" );
	 return PAPI_ESYS;
      }

      if ( cnt == 0 ) {
         /* We read 0 bytes if we could not schedule the event */
         /* The kernel should have detected this at open       */
         /* but various bugs (including NMI watchdog)          */
         /* result in this behavior                            */

	 return PAPI_ECNFLCT;

     } else {

	/* Reset all of the counters (opened so far) back to zero      */
	/* from the above brief enable/disable call pair.              */

	/* We have to reset all events because reset of group leader      */
        /* does not reset all.                                            */
	/* we assume that the events are being added one by one and that  */
        /* we do not need to reset higher events (doing so may reset ones */
        /* that have not been initialized yet.                            */

	/* Note... PERF_EVENT_IOC_RESET does not reset time running       */
	/* info if multiplexing, so we should avoid coming here if        */
	/* we are multiplexing the event.                                 */
        for( i = 0; i < idx; i++) {
	   retval=ioctl( ctl->events[i].event_fd, PERF_EVENT_IOC_RESET, NULL );
	   if (retval == -1) {
	      PAPIERROR( "ioctl(PERF_EVENT_IOC_RESET) #%d/%d %d "
			 "(fd %d)failed.\n",
			 i,ctl->num_events,idx,ctl->events[i].event_fd);
	      return PAPI_ESYS;
	   }
	}
      }
   }
   return PAPI_OK;
}


/* Do some extra work on a perf_event fd if we're doing sampling  */
/* This mostly means setting up the mmap buffer.                  */
static int
tune_up_fd( pe_control_t *ctl, int evt_idx )
{
   int ret;
   void *buf_addr;
   int fd = ctl->events[evt_idx].event_fd;

   /* Register that we would like a SIGIO notification when a mmap'd page */
   /* becomes full.                                                       */
   ret = fcntl( fd, F_SETFL, O_ASYNC | O_NONBLOCK );
   if ( ret ) {
      PAPIERROR ( "fcntl(%d, F_SETFL, O_ASYNC | O_NONBLOCK) "
		  "returned error: %s", fd, strerror( errno ) );
      return PAPI_ESYS;
   }

   /* Set the F_SETOWN_EX flag on the fd.                          */
   /* This affects which thread an overflow signal gets sent to.   */
   ret=fcntl_setown_fd(fd);
   if (ret!=PAPI_OK) return ret;
	   
   /* Set FD_CLOEXEC.  Otherwise if we do an exec with an overflow */
   /* running, the overflow handler will continue into the exec()'d*/
   /* process and kill it because no signal handler is set up.     */
   ret=fcntl(fd, F_SETFD, FD_CLOEXEC);
   if (ret) {
      return PAPI_ESYS;
   }

   /* when you explicitely declare that you want a particular signal,  */
   /* even with you use the default signal, the kernel will send more  */
   /* information concerning the event to the signal handler.          */
   /*                                                                  */
   /* In particular, it will send the file descriptor from which the   */
   /* event is originating which can be quite useful when monitoring   */
   /* multiple tasks from a single thread.                             */
   ret = fcntl( fd, F_SETSIG, ctl->overflow_signal );
   if ( ret == -1 ) {
      PAPIERROR( "cannot fcntl(F_SETSIG,%d) on %d: %s",
		 ctl->overflow_signal, fd,
		 strerror( errno ) );
      return PAPI_ESYS;
   }

   /* mmap() the sample buffer */
   buf_addr = mmap( NULL, ctl->events[evt_idx].nr_mmap_pages * getpagesize(),
		    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
   if ( buf_addr == MAP_FAILED ) {
      PAPIERROR( "mmap(NULL,%d,%d,%d,%d,0): %s",
		 ctl->events[evt_idx].nr_mmap_pages * getpagesize(  ), 
		 PROT_READ, MAP_SHARED, fd, strerror( errno ) );
      return ( PAPI_ESYS );
   }

   SUBDBG( "Sample buffer for fd %d is located at %p\n", fd, buf_addr );

   /* Set up the mmap buffer and its associated helpers */
   ctl->events[evt_idx].mmap_buf = (struct perf_counter_mmap_page *) buf_addr;
   ctl->events[evt_idx].tail = 0;
   ctl->events[evt_idx].mask = ( ctl->events[evt_idx].nr_mmap_pages - 1 ) * 
                               getpagesize() - 1;

   return PAPI_OK;
}


/* Open all events in the control state */
static int
open_pe_events( pe_context_t *ctx, pe_control_t *ctl )
{

   int i, ret = PAPI_OK;
   long pid;

   if (ctl->granularity==PAPI_GRN_SYS) {
      pid = -1;
   }
   else {
      pid = ctl->tid;
   }

   for( i = 0; i < ctl->num_events; i++ ) {

      ctl->events[i].event_opened=0;

      /* set up the attr structure.  We don't set up all fields here */
      /* as some have already been set up previously.                */

      /* group leader (event 0) is special                */
      /* If we're multiplexed, everyone is a group leader */
      if (( i == 0 ) || (ctl->multiplexed)) {
         ctl->events[i].attr.pinned = !ctl->multiplexed;
	 ctl->events[i].attr.disabled = 1;
	 ctl->events[i].group_leader_fd=-1;
         ctl->events[i].attr.read_format = get_read_format(ctl->multiplexed, 
							   ctl->inherit, 
							   !ctl->multiplexed );
      } else {
	 ctl->events[i].attr.pinned=0;
	 ctl->events[i].attr.disabled = 0;
	 ctl->events[i].group_leader_fd=ctl->events[0].event_fd;
         ctl->events[i].attr.read_format = get_read_format(ctl->multiplexed, 
							   ctl->inherit, 
							   0 );
      }


      /* try to open */
      ctl->events[i].event_fd = sys_perf_event_open( &ctl->events[i].attr, 
						     pid,
						     ctl->cpu,
			       ctl->events[i].group_leader_fd,
						     0 /* flags */
						     );
      
      /* Try to match Linux errors to PAPI errors */
      if ( ctl->events[i].event_fd == -1 ) {
	 SUBDBG("sys_perf_event_open returned error on event #%d."
		"  Error: %s\n",
		i, strerror( errno ) );
         ret=map_perf_event_errors_to_papi(errno);

	 goto open_pe_cleanup;
      }

      SUBDBG ("sys_perf_event_open: tid: %ld, cpu_num: %d,"
              " group_leader/fd: %d, event_fd: %d,"
              " read_format: 0x%"PRIu64"\n",
	      pid, ctl->cpu, ctl->events[i].group_leader_fd, 
	      ctl->events[i].event_fd, ctl->events[i].attr.read_format);


      /* in many situations the kernel will indicate we opened fine */
      /* yet things will fail later.  So we need to double check    */
      /* we actually can use the events we've set up.               */

      /* This is not necessary if we are multiplexing, and in fact */
      /* we cannot do this properly if multiplexed because         */
      /* PERF_EVENT_IOC_RESET does not reset the time running info */
      if (!ctl->multiplexed) {
	 ret = check_scheduability( ctx, ctl, i );

         if ( ret != PAPI_OK ) {
	    /* the last event did open, so we need to bump the counter */
	    /* before doing the cleanup                                */
	    i++;
		                          
            goto open_pe_cleanup;
	 }
      }
      ctl->events[i].event_opened=1;
   }

   /* Now that we've successfully opened all of the events, do whatever  */
   /* "tune-up" is needed to attach the mmap'd buffers, signal handlers, */
   /* and so on.                                                         */
   for ( i = 0; i < ctl->num_events; i++ ) {

      /* If sampling is enabled, hook up signal handler */
      if ( ctl->events[i].attr.sample_period ) {
	 ret = tune_up_fd( ctl, i );
	 if ( ret != PAPI_OK ) {
	    /* All of the fds are open, so we need to clean up all of them */
	    i = ctl->num_events;
	    goto open_pe_cleanup;
	 }
      } else {
	 /* Make sure this is NULL so close_pe_events works right */
	 ctl->events[i].mmap_buf = NULL;
      }
   }

   /* Set num_evts only if completely successful */
   ctx->state |= PERF_EVENTS_OPENED;
		
   return PAPI_OK;

open_pe_cleanup:
   /* We encountered an error, close up the fds we successfully opened.  */
   /* We go backward in an attempt to close group leaders last, although */
   /* That's probably not strictly necessary.                            */
   while ( i > 0 ) {
      i--;
      if (ctl->events[i].event_fd>=0) {
	 close( ctl->events[i].event_fd );
	 ctl->events[i].event_opened=0;
      }
   }

   return ret;
}

/* Close all of the opened events */
static int
close_pe_events( pe_context_t *ctx, pe_control_t *ctl )
{
   int i;
   int num_closed=0;
   int events_not_opened=0;

   /* should this be a more serious error? */
   if ( ctx->state & PERF_EVENTS_RUNNING ) {
      SUBDBG("Closing without stopping first\n");
   }

   /* Close child events first */
   for( i=0; i<ctl->num_events; i++ ) {

      if (ctl->events[i].event_opened) {

         if (ctl->events[i].group_leader_fd!=-1) {
            if ( ctl->events[i].mmap_buf ) {
	       if ( munmap ( ctl->events[i].mmap_buf,
		             ctl->events[i].nr_mmap_pages * getpagesize() ) ) {
	          PAPIERROR( "munmap of fd = %d returned error: %s",
			     ctl->events[i].event_fd, strerror( errno ) );
	          return PAPI_ESYS;
	       }
	    }

            if ( close( ctl->events[i].event_fd ) ) {
	       PAPIERROR( "close of fd = %d returned error: %s",
		       ctl->events[i].event_fd, strerror( errno ) );
	       return PAPI_ESYS;
	    } else {
	       num_closed++;
	    }
	    ctl->events[i].event_opened=0;
	 }
      }
      else {
	events_not_opened++;
      }
   }

   /* Close the group leaders last */
   for( i=0; i<ctl->num_events; i++ ) {

      if (ctl->events[i].event_opened) {

         if (ctl->events[i].group_leader_fd==-1) {
            if ( ctl->events[i].mmap_buf ) {
	       if ( munmap ( ctl->events[i].mmap_buf,
		             ctl->events[i].nr_mmap_pages * getpagesize() ) ) {
	          PAPIERROR( "munmap of fd = %d returned error: %s",
			     ctl->events[i].event_fd, strerror( errno ) );
	          return PAPI_ESYS;
	       }
	    }


            if ( close( ctl->events[i].event_fd ) ) {
	       PAPIERROR( "close of fd = %d returned error: %s",
		       ctl->events[i].event_fd, strerror( errno ) );
	       return PAPI_ESYS;
	    } else {
	       num_closed++;
	    }
	    ctl->events[i].event_opened=0;
	 }
      }
   }


   if (ctl->num_events!=num_closed) {
      if (ctl->num_events!=(num_closed+events_not_opened)) {
         PAPIERROR("Didn't close all events: "
		   "Closed %d Not Opened: %d Expected %d\n",
		   num_closed,events_not_opened,ctl->num_events);
         return PAPI_EBUG;
      }
   }

   ctl->num_events=0;

   ctx->state &= ~PERF_EVENTS_OPENED;

   return PAPI_OK;
}


/********************************************************************/
/********************************************************************/
/* Start with functions that are exported via the module interface  */
/********************************************************************/
/********************************************************************/


/* set the domain. FIXME: perf_events allows per-event control of this. */
/* we do not handle that yet.                                           */
int
_pe_set_domain( hwd_control_state_t *ctl, int domain)
{
	
   int i;
   pe_control_t *pe_ctl = ( pe_control_t *) ctl;

   SUBDBG("old control domain %d, new domain %d\n",
	  pe_ctl->domain,domain);

   pe_ctl->domain = domain;
     
   /* Force the domain on all events */
   for( i = 0; i < pe_ctl->num_events; i++ ) {
      pe_ctl->events[i].attr.exclude_user = 
	                !( pe_ctl->domain & PAPI_DOM_USER );
      pe_ctl->events[i].attr.exclude_kernel =
			!( pe_ctl->domain & PAPI_DOM_KERNEL );
      pe_ctl->events[i].attr.exclude_hv =
			!( pe_ctl->domain & PAPI_DOM_SUPERVISOR );
   }
   return PAPI_OK;
}

/* Shutdown a thread */
int
_pe_shutdown_thread( hwd_context_t *ctx )
{
    pe_context_t *pe_ctx = ( pe_context_t *) ctx;

    pe_ctx->initialized=0;

    return PAPI_OK;
}


/* reset the hardware counters */
/* Note: PAPI_reset() does not necessarily call this */
/* unless the events are actually running.           */
int
_pe_reset( hwd_context_t *ctx, hwd_control_state_t *ctl )
{
   int i, ret;
   pe_control_t *pe_ctl = ( pe_control_t *) ctl;

   ( void ) ctx;			 /*unused */

   /* We need to reset all of the events, not just the group leaders */
   for( i = 0; i < pe_ctl->num_events; i++ ) {
      ret = ioctl( pe_ctl->events[i].event_fd, PERF_EVENT_IOC_RESET, NULL );
      if ( ret == -1 ) {
	 PAPIERROR("ioctl(%d, PERF_EVENT_IOC_RESET, NULL) "
		   "returned error, Linux says: %s",
		   pe_ctl->events[i].event_fd, strerror( errno ) );
	 return PAPI_ESYS;
      }
   }

   return PAPI_OK;
}


/* write (set) the hardware counters */
/* Current we do not support this.   */
int
_pe_write( hwd_context_t *ctx, hwd_control_state_t *ctl,
		long long *from )
{
   ( void ) ctx;			 /*unused */
   ( void ) ctl;			 /*unused */
   ( void ) from;			 /*unused */
   /*
    * Counters cannot be written.  Do we need to virtualize the
    * counters so that they can be written, or perhaps modify code so that
    * they can be written? FIXME ?
    */
	
    return PAPI_ENOSUPP;
}

/*
 * perf_event provides a complicated read interface.
 *  the info returned by read() varies depending on whether
 *  you have PERF_FORMAT_GROUP, PERF_FORMAT_TOTAL_TIME_ENABLED,
 *  PERF_FORMAT_TOTAL_TIME_RUNNING, or PERF_FORMAT_ID set
 *
 * To simplify things we just always ask for everything.  This might
 * lead to overhead when reading more than we need, but it makes the
 * read code a lot simpler than the original implementation we had here.
 *
 * For more info on the layout see include/linux/perf_event.h
 *
 */

int
_pe_read( hwd_context_t *ctx, hwd_control_state_t *ctl,
	       long long **events, int flags )
{
   ( void ) flags;			 /*unused */
   int i, ret = -1;
   pe_context_t *pe_ctx = ( pe_context_t *) ctx;
   pe_control_t *pe_ctl = ( pe_control_t *) ctl;
   long long papi_pe_buffer[READ_BUFFER_SIZE];
   long long tot_time_running, tot_time_enabled, scale;

   /* On kernels before 2.6.33 the TOTAL_TIME_ENABLED and TOTAL_TIME_RUNNING */
   /* fields are always 0 unless the counter is disabled.  So if we are on   */
   /* one of these kernels, then we must disable events before reading.      */

   /* Elsewhere though we disable multiplexing on kernels before 2.6.34 */
   /* so maybe this isn't even necessary.                               */

   if (bug_sync_read()) {
      if ( pe_ctx->state & PERF_EVENTS_RUNNING ) {
         for ( i = 0; i < pe_ctl->num_events; i++ ) {
	    /* disable only the group leaders */
	    if ( pe_ctl->events[i].group_leader_fd == -1 ) {
	       ret = ioctl( pe_ctl->events[i].event_fd, 
			   PERF_EVENT_IOC_DISABLE, NULL );
	       if ( ret == -1 ) {
	          PAPIERROR("ioctl(PERF_EVENT_IOC_DISABLE) "
			   "returned an error: ", strerror( errno ));
	          return PAPI_ESYS;
	       }
	    }
	 }
      }
   }


   /* Handle case where we are multiplexing */
   if (pe_ctl->multiplexed) {

      /* currently we handle multiplexing by having individual events */
      /* so we read from each in turn.                                */

      for ( i = 0; i < pe_ctl->num_events; i++ ) {
             
         ret = read( pe_ctl->events[i].event_fd, papi_pe_buffer, 
		    sizeof ( papi_pe_buffer ) );
         if ( ret == -1 ) {
	    PAPIERROR("read returned an error: ", strerror( errno ));
	    return PAPI_ESYS;
	 }

	 /* We should read 3 64-bit values from the counter */
	 if (ret<(signed)(3*sizeof(long long))) {
	    PAPIERROR("Error!  short read!\n");	 
	    return PAPI_ESYS;
	 }        

         SUBDBG("read: fd: %2d, tid: %ld, cpu: %d, ret: %d\n", 
	        pe_ctl->events[i].event_fd, 
		(long)pe_ctl->tid, pe_ctl->cpu, ret);
         SUBDBG("read: %lld %lld %lld\n",papi_pe_buffer[0],
	        papi_pe_buffer[1],papi_pe_buffer[2]);

         tot_time_enabled = papi_pe_buffer[1];     
         tot_time_running = papi_pe_buffer[2];

         SUBDBG("count[%d] = (papi_pe_buffer[%d] %lld * "
		"tot_time_enabled %lld) / tot_time_running %lld\n",
		i, 0,papi_pe_buffer[0],
		tot_time_enabled,tot_time_running);
    
         if (tot_time_running == tot_time_enabled) {
	    /* No scaling needed */
	    pe_ctl->counts[i] = papi_pe_buffer[0];
         } else if (tot_time_running && tot_time_enabled) {
	    /* Scale factor of 100 to avoid overflows when computing */
	    /*enabled/running */

	    scale = (tot_time_enabled * 100LL) / tot_time_running;
	    scale = scale * papi_pe_buffer[0];
	    scale = scale / 100LL;
	    pe_ctl->counts[i] = scale;
	 } else {
	   /* This should not happen, but Phil reports it sometime does. */
	    SUBDBG("perf_event kernel bug(?) count, enabled, "
		   "running: %lld, %lld, %lld\n",
		   papi_pe_buffer[0],tot_time_enabled,
		   tot_time_running);

	    pe_ctl->counts[i] = papi_pe_buffer[0];
	 }
      }
   }

   /* Handle cases where we cannot use FORMAT GROUP */
   else if (bug_format_group() || pe_ctl->inherit) {

      /* we must read each counter individually */
      for ( i = 0; i < pe_ctl->num_events; i++ ) {

         ret = read( pe_ctl->events[i].event_fd, papi_pe_buffer, 
		    sizeof ( papi_pe_buffer ) );
         if ( ret == -1 ) {
	    PAPIERROR("read returned an error: ", strerror( errno ));
	    return PAPI_ESYS;
	 }

	 /* we should read one 64-bit value from each counter */
	 if (ret!=sizeof(long long)) {
	    PAPIERROR("Error!  short read!\n");
	    PAPIERROR("read: fd: %2d, tid: %ld, cpu: %d, ret: %d\n",
		   pe_ctl->events[i].event_fd,
		   (long)pe_ctl->tid, pe_ctl->cpu, ret);
	    return PAPI_ESYS;
	 }     

         SUBDBG("read: fd: %2d, tid: %ld, cpu: %d, ret: %d\n", 
	        pe_ctl->events[i].event_fd, (long)pe_ctl->tid, 
		pe_ctl->cpu, ret);
         SUBDBG("read: %lld\n",papi_pe_buffer[0]);
     
	 pe_ctl->counts[i] = papi_pe_buffer[0];
      }
   }

   
   /* Handle cases where we are using FORMAT_GROUP   */
   /* We assume only one group leader, in position 0 */

   else {
      if (pe_ctl->events[0].group_leader_fd!=-1) {
	 PAPIERROR("Was expecting group leader!\n");
      }

      ret = read( pe_ctl->events[0].event_fd, papi_pe_buffer, 
		  sizeof ( papi_pe_buffer ) );

      if ( ret == -1 ) {
	 PAPIERROR("read returned an error: ", strerror( errno ));
	 return PAPI_ESYS;
      }

      /* we read 1 64-bit value (number of events) then     */
      /* num_events more 64-bit values that hold the counts */
      if (ret<(signed)((1+pe_ctl->num_events)*sizeof(long long))) {
	 PAPIERROR("Error! short read!\n");
	 return PAPI_ESYS;
      }

      SUBDBG("read: fd: %2d, tid: %ld, cpu: %d, ret: %d\n", 
	     pe_ctl->events[0].event_fd, 
	     (long)pe_ctl->tid, pe_ctl->cpu, ret);
      { 
	 int j;
	 for(j=0;j<ret/8;j++) {
            SUBDBG("read %d: %lld\n",j,papi_pe_buffer[j]);
	 }
      }

      /* Make sure the kernel agrees with how many events we have */
      if (papi_pe_buffer[0]!=pe_ctl->num_events) {
	 PAPIERROR("Error!  Wrong number of events!\n");
	 return PAPI_ESYS;
      }

      /* put the count values in their proper location */
      for(i=0;i<papi_pe_buffer[0];i++) {
         pe_ctl->counts[i] = papi_pe_buffer[1+i];
      }
   }


   /* If we disabled the counters due to the sync_read_bug(), */
   /* then we need to re-enable them now.                     */
   if (bug_sync_read()) {
      if ( pe_ctx->state & PERF_EVENTS_RUNNING ) {
         for ( i = 0; i < pe_ctl->num_events; i++ ) {
	    if ( pe_ctl->events[i].group_leader_fd == -1 ) {
	       /* this should refresh any overflow counters too */
	       ret = ioctl( pe_ctl->events[i].event_fd, 
			    PERF_EVENT_IOC_ENABLE, NULL );
	       if ( ret == -1 ) {
	          /* Should never happen */
	          PAPIERROR("ioctl(PERF_EVENT_IOC_ENABLE) returned an error: ",
			    strerror( errno ));
	          return PAPI_ESYS;
	       }
	    }
	 }
      }
   }

   /* point PAPI to the values we read */
   *events = pe_ctl->counts;

   return PAPI_OK;
}

/* Start counting events */
int
_pe_start( hwd_context_t *ctx, hwd_control_state_t *ctl )
{
   int ret;
   int i;
   int did_something = 0;
   pe_context_t *pe_ctx = ( pe_context_t *) ctx;
   pe_control_t *pe_ctl = ( pe_control_t *) ctl;

   /* Reset the counters first.  Is this necessary? */
   ret = _pe_reset( pe_ctx, pe_ctl );
   if ( ret ) {
      return ret;
   }

   /* Enable all of the group leaders                */
   /* All group leaders have a group_leader_fd of -1 */
   for( i = 0; i < pe_ctl->num_events; i++ ) {
      if (pe_ctl->events[i].group_leader_fd == -1) {
	 SUBDBG("ioctl(enable): fd: %d\n", pe_ctl->events[i].event_fd);
	 ret=ioctl( pe_ctl->events[i].event_fd, PERF_EVENT_IOC_ENABLE, NULL) ; 

	 /* ioctls always return -1 on failure */
         if (ret == -1) {
            PAPIERROR("ioctl(PERF_EVENT_IOC_ENABLE) failed.\n");
            return PAPI_ESYS;
	 }

	 did_something++;
      } 
   }

   if (!did_something) {
      PAPIERROR("Did not enable any counters.\n");
      return PAPI_EBUG;
   }

   pe_ctx->state |= PERF_EVENTS_RUNNING;

   return PAPI_OK;

}

/* Stop all of the counters */
int
_pe_stop( hwd_context_t *ctx, hwd_control_state_t *ctl )
{
	
   int ret;
   int i;
   pe_context_t *pe_ctx = ( pe_context_t *) ctx;
   pe_control_t *pe_ctl = ( pe_control_t *) ctl;

   /* Just disable the group leaders */
   for ( i = 0; i < pe_ctl->num_events; i++ ) {
      if ( pe_ctl->events[i].group_leader_fd == -1 ) {
	 ret=ioctl( pe_ctl->events[i].event_fd, PERF_EVENT_IOC_DISABLE, NULL);
	 if ( ret == -1 ) {
	    PAPIERROR( "ioctl(%d, PERF_EVENT_IOC_DISABLE, NULL) "
		       "returned error, Linux says: %s",
		       pe_ctl->events[i].event_fd, strerror( errno ) );
	    return PAPI_EBUG;
	 }
      }
   }

   pe_ctx->state &= ~PERF_EVENTS_RUNNING;

   return PAPI_OK;
}


/* This function clears the current contents of the control structure and
   updates it with whatever resources are allocated for all the native events
   in the native info structure array. */

int
_pe_update_control_state( hwd_control_state_t *ctl, 
			       NativeInfo_t *native,
			       int count, hwd_context_t *ctx )
{
   int i = 0, ret;
   pe_context_t *pe_ctx = ( pe_context_t *) ctx;
   pe_control_t *pe_ctl = ( pe_control_t *) ctl;

   /* close all of the existing fds and start over again */
   /* In theory we could have finer-grained control and know if             */
   /* things were changed, but it's easier to tear things down and rebuild. */
   close_pe_events( pe_ctx, pe_ctl );

   /* Calling with count==0 should be OK, it's how things are deallocated */
   /* when an eventset is destroyed.                                      */
   if ( count == 0 ) {
      SUBDBG( "Called with count == 0\n" );
      return PAPI_OK;
   }

   /* set up all the events */
   for( i = 0; i < count; i++ ) {
      if ( native ) {
	 /* Have libpfm4 set the config values for the event */
	 ret=_papi_libpfm4_setup_counters(&pe_ctl->events[i].attr,
					native[i].ni_event,
					pe_ctx->event_table);
	 SUBDBG( "pe_ctl->eventss[%d].config=%"PRIx64"\n",i,
		 pe_ctl->events[i].attr.config);
	 if (ret!=PAPI_OK) return ret;

      } else {
	  /* I'm not sure how we'd end up in this case */
          /* should it be an error?                    */
      }

      /* Copy the inherit flag into the attribute block that will be   */
      /* passed to the kernel */
      pe_ctl->events[i].attr.inherit = pe_ctl->inherit;

      /* Set the position in the native structure */
      /* We just set up events linearly           */
      if ( native ) {
	 native[i].ni_position = i;
      }
   }

   pe_ctl->num_events = count;
   _pe_set_domain( ctl, pe_ctl->domain );

   /* actuall open the events */
   /* (why is this a separate function?) */
   ret = open_pe_events( pe_ctx, pe_ctl );
   if ( ret != PAPI_OK ) {
      SUBDBG("open_pe_events failed\n");
      /* Restore values ? */
      return ret;
   }

   return PAPI_OK;
}

/* Set various options on a control state */
int
_pe_ctl( hwd_context_t *ctx, int code, _papi_int_option_t *option )
{
   int ret;
   pe_context_t *pe_ctx = ( pe_context_t *) ctx;
   pe_control_t *pe_ctl = NULL;

   switch ( code ) {
      case PAPI_MULTIPLEX:
	   pe_ctl = ( pe_control_t * ) ( option->multiplex.ESI->ctl_state );
	   ret = check_permissions( pe_ctl->tid, pe_ctl->cpu, pe_ctl->domain,
				    pe_ctl->granularity,
				    1, pe_ctl->inherit );
           if (ret != PAPI_OK) {
	      return ret;
	   }

	   /* looks like we are allowed, so set multiplexed attribute */
	   pe_ctl->multiplexed = 1;
	   ret = _pe_update_control_state( pe_ctl, NULL, 
						pe_ctl->num_events, pe_ctx );
	   if (ret != PAPI_OK) {
	      pe_ctl->multiplexed = 0;
	   }
	   return ret;
	
      case PAPI_ATTACH:
	   pe_ctl = ( pe_control_t * ) ( option->attach.ESI->ctl_state );
	   ret = check_permissions( option->attach.tid, pe_ctl->cpu, 
				  pe_ctl->domain, pe_ctl->granularity,
				  pe_ctl->multiplexed, 
				    pe_ctl->inherit );
	   if (ret != PAPI_OK) {
	      return ret;
	   }

	   pe_ctl->tid = option->attach.tid;

	   /* If events have been already been added, something may */
	   /* have been done to the kernel, so update */
	   ret =_pe_update_control_state( pe_ctl, NULL, 
						pe_ctl->num_events, pe_ctx);
	   
	   return ret;

      case PAPI_DETACH:
	   pe_ctl = ( pe_control_t *) ( option->attach.ESI->ctl_state );

	   pe_ctl->tid = 0;
	   return PAPI_OK;

      case PAPI_CPU_ATTACH:
	   pe_ctl = ( pe_control_t *) ( option->cpu.ESI->ctl_state );
	   ret = check_permissions( pe_ctl->tid, option->cpu.cpu_num, 
				    pe_ctl->domain, pe_ctl->granularity,
				    pe_ctl->multiplexed, 
				    pe_ctl->inherit );
           if (ret != PAPI_OK) {
	       return ret;
	   }
	   /* looks like we are allowed so set cpu number */

	   /* this tells the kernel not to count for a thread   */
	   /* should we warn if we try to set both?  perf_event */
	   /* will reject it.                                   */
	   pe_ctl->tid = -1;      

	   pe_ctl->cpu = option->cpu.cpu_num;

	   return PAPI_OK;

      case PAPI_DOMAIN:
	   pe_ctl = ( pe_control_t *) ( option->domain.ESI->ctl_state );
	   ret = check_permissions( pe_ctl->tid, pe_ctl->cpu, 
				    option->domain.domain,
				    pe_ctl->granularity,
				    pe_ctl->multiplexed,
				    pe_ctl->inherit );
           if (ret != PAPI_OK) {
	      return ret;
	   }
	   /* looks like we are allowed, so set counting domain */
	   return _pe_set_domain( pe_ctl, option->domain.domain );

      case PAPI_GRANUL:
	   pe_ctl = (pe_control_t *) ( option->granularity.ESI->ctl_state );

	   /* FIXME: we really don't support this yet */

           switch ( option->granularity.granularity  ) {
              case PAPI_GRN_PROCG:
              case PAPI_GRN_SYS_CPU:
              case PAPI_GRN_PROC:
		   return PAPI_ECMP;
     
	      /* Currently we only support thread and CPU granularity */
              case PAPI_GRN_SYS:
	 	   pe_ctl->granularity=PAPI_GRN_SYS;
		   break;

              case PAPI_GRN_THR:
	 	   pe_ctl->granularity=PAPI_GRN_THR;
		   break;


              default:
		   return PAPI_EINVAL;
	   }
           return PAPI_OK;

      case PAPI_INHERIT:
	   pe_ctl = (pe_control_t *) ( option->inherit.ESI->ctl_state );
	   ret = check_permissions( pe_ctl->tid, pe_ctl->cpu, pe_ctl->domain, 
				  pe_ctl->granularity, pe_ctl->multiplexed, 
				    option->inherit.inherit );
           if (ret != PAPI_OK) {
	      return ret;
	   }
	   /* looks like we are allowed, so set the requested inheritance */
	   if (option->inherit.inherit) {
	      /* children will inherit counters */
	      pe_ctl->inherit = 1;
	   } else {
	      /* children won't inherit counters */
	      pe_ctl->inherit = 0;
	   }
	   return PAPI_OK;

      case PAPI_DATA_ADDRESS:
	   return PAPI_ENOSUPP;
#if 0
	   pe_ctl = (pe_control_t *) (option->address_range.ESI->ctl_state);
	   ret = set_default_domain( pe_ctl, option->address_range.domain );
	   if ( ret != PAPI_OK ) {
	      return ret;
	   }
	   set_drange( pe_ctx, pe_ctl, option );
	   return PAPI_OK;
#endif
      case PAPI_INSTR_ADDRESS:
	   return PAPI_ENOSUPP;
#if 0
	   pe_ctl = (pe_control_t *) (option->address_range.ESI->ctl_state);
	   ret = set_default_domain( pe_ctl, option->address_range.domain );
	   if ( ret != PAPI_OK ) {
	      return ret;
	   }
	   set_irange( pe_ctx, pe_ctl, option );
	   return PAPI_OK;
#endif

      case PAPI_DEF_ITIMER:
	   /* What should we be checking for here?                   */
	   /* This seems like it should be OS-specific not component */
	   /* specific.                                              */

	   return PAPI_OK;
	
      case PAPI_DEF_MPX_NS:
	   /* Defining a given ns per set is not current supported */
	   return PAPI_ENOSUPP;
	
      case PAPI_DEF_ITIMER_NS:
	   /* We don't support this... */
	   return PAPI_OK;
	
      default:
	   return PAPI_ENOSUPP;
   }
}

/* In case headers aren't new enough to have __NR_perf_event_open */
#ifndef __NR_perf_event_open

#ifdef __powerpc__
#define __NR_perf_event_open	319
#elif defined(__x86_64__)
#define __NR_perf_event_open	298
#elif defined(__i386__)
#define __NR_perf_event_open	336
#elif defined(__arm__)          366+0x900000
#define __NR_perf_event_open    
#endif

#endif

long
sys_perf_event_open( struct perf_event_attr *hw_event, pid_t pid, int cpu,
					   int group_fd, unsigned long flags )
{
	int ret;

   SUBDBG("sys_perf_event_open(%p,%d,%d,%d,%lx\n",hw_event,pid,cpu,group_fd,flags);
   SUBDBG("   type: %d\n",hw_event->type);
   SUBDBG("   size: %d\n",hw_event->size);
   SUBDBG("   config: %"PRIx64" (%"PRIu64")\n",hw_event->config,
	  hw_event->config);
   SUBDBG("   sample_period: %"PRIu64"\n",hw_event->sample_period);
   SUBDBG("   sample_type: %"PRIu64"\n",hw_event->sample_type);
   SUBDBG("   read_format: %"PRIu64"\n",hw_event->read_format);
   SUBDBG("   disabled: %d\n",hw_event->disabled);
   SUBDBG("   inherit: %d\n",hw_event->inherit);
   SUBDBG("   pinned: %d\n",hw_event->pinned);
   SUBDBG("   exclusive: %d\n",hw_event->exclusive);
   SUBDBG("   exclude_user: %d\n",hw_event->exclude_user);
   SUBDBG("   exclude_kernel: %d\n",hw_event->exclude_kernel);
   SUBDBG("   exclude_hv: %d\n",hw_event->exclude_hv);
   SUBDBG("   exclude_idle: %d\n",hw_event->exclude_idle);   
   SUBDBG("   mmap: %d\n",hw_event->mmap);   
   SUBDBG("   comm: %d\n",hw_event->comm);   
   SUBDBG("   freq: %d\n",hw_event->freq);   
   SUBDBG("   inherit_stat: %d\n",hw_event->inherit_stat);   
   SUBDBG("   enable_on_exec: %d\n",hw_event->enable_on_exec);   
   SUBDBG("   task: %d\n",hw_event->task);   
   SUBDBG("   watermark: %d\n",hw_event->watermark);   
   
   
	ret =
		syscall( __NR_perf_event_open, hw_event, pid, cpu, group_fd, flags );
	SUBDBG("Returned %d %d %s\n",ret,
	       ret<0?errno:0,
	       ret<0?strerror(errno):" ");
	return ret;
}