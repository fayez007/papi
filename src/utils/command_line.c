/* 
 * This simply tries to add the events listed on the command line one at a time
 * then starts and stops the counters and prints the results
*/

#include "papi_test.h"

int main(int argc, char **argv)
{
   int retval;
   int num_events;
   long_long *values;
   char *success;
   int EventSet = PAPI_NULL;
   int i, j, event;
   char errstr[PAPI_HUGE_STR_LEN];

   tests_quiet(argc, argv);     /* Set TESTS_QUIET variable */


   if ((retval = PAPI_library_init(PAPI_VER_CURRENT)) != PAPI_VER_CURRENT)
      test_fail(__FILE__, __LINE__, "PAPI_library_init", retval);

   if ((retval = PAPI_create_eventset(&EventSet)) != PAPI_OK)
      test_fail(__FILE__, __LINE__, "PAPI_create_eventset", retval);

   if ( TESTS_QUIET ) 
     i = 2;
   else
     i = 1;

   num_events = argc-i;

   /* Automatically pass if no events, for run_tests.sh */
   if ( num_events == 0 ) 
     test_pass(__FILE__, NULL, 0);

   values = (long_long *) malloc(sizeof(long_long)*num_events);
   success = (char *) malloc(argc);

   if ( success == NULL || values == NULL ) 
      test_fail(__FILE__, __LINE__, "malloc", PAPI_ESYS);

   for ( ;i<argc; i++ ){
     if ( (retval = PAPI_event_name_to_code(argv[i], &event)) != PAPI_OK )
       test_fail(__FILE__, __LINE__, "PAPI_event_name_to_code", retval);

     if ( (retval = PAPI_add_event(EventSet, event)) != PAPI_OK ) {
         PAPI_perror(retval, errstr, 1024 );
         printf("Failed adding: %s\nbecause: %s\n", argv[i], errstr );
        success[i] = 0;
     }
     else {
        success[i] = 1;
        printf("Successfully added: %s\n", argv[i]);
     }
   }
   printf("\n");

   do_flops(1);
   do_flush();

   if ((retval = PAPI_start(EventSet)) != PAPI_OK)
      test_fail(__FILE__, __LINE__, "PAPI_start", retval);

   do_flops(NUM_FLOPS);
   do_misses(1,L1_MISS_BUFFER_SIZE_INTS);

   if ((retval = PAPI_stop(EventSet, values)) != PAPI_OK)
      test_fail(__FILE__, __LINE__, "PAPI_stop", retval);

   for(i=1, j=0; i<argc; i++ ) {
      if (success[i]) {
         printf("%s : \t%lld\n", argv[i], values[j++]);
      } else {
         printf("%s : \t---------\n", argv[i]);
      }
   }

   printf("\n----------------------------------\n");
   printf("Verification: None.\n This utility lets you add events from the command line interface to see if they work.\n");
   test_pass(__FILE__, NULL, 0);
   exit(1);
}