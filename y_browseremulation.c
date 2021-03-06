/*
 * Ylib Loadrunner function library.
 * Copyright (C) 2012 Floris Kraak <randakar@gmail.com> | <fkraak@ymor.nl>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#ifndef _Y_BROWSER_EMULATION_C
#define _Y_BROWSER_EMULATION_C

#include "y_string.c"
// #include "y_loadrunner_utils.c"

/*-------------------------------------------------------------
Initial code for supporting (more) realistic browser emulation.

THIS CODE IS STILL EXPERIMENTAL
Feel free to use it, but I reserve the right to break things. Especially the data format, as that one is liable to change some time soon.

Basic concept: The tester defines a list of browsers inside a parameter file, that contains:
1) browser name
2) browser chance - A count of how many times said browser was seen in a production access log during peak hours should work. Percentages aren't required or even very helpfull.
3) max connections - How many connections said browser uses (see: www.browserscope.org for what this value should be for your browser)
4) max connections per host - Same as above, but this setting limits the number of connections to a single hostname.
5) User agent string - A user agent string used by said browser.

The last entry of this list of parameters needs to use "END" as the browser name.

This list is then read in using lr_advance_param() during vuser_init() to construct a list of browsers, which can then be used during the loadtest to dynamically choose browsers to emulate based on their likelyhood
that said browser is seen in production.

Usage:

1) Set up the appropriate parameters
2) Add this file to your script, plus any supporting ylib files.
3) Add an include statement: '#include "y_browseremulation.c' to the top of vuser_init()

4) Call y_setup_browser_emulation in vuser_init()

    Example:

    vuser_init()
    {
        y_log_turn_off();
        y_setup_browser_emulation();
        y_log_restore();
    }

5) Call y_choose_browser() and y_emulate_browser() at the start of each iteration.

    Example:

    Action()
    {
        // This will set up some connection options for emulating different browser versions in a realistic fashion
        {
            // Note: This will blow up if the setup code hasn't been called beforehand.
            y_browser* browser = y_choose_browser();
            y_emulate_browser(browser);
        }
    
    // .. more code ..
    
    }

6) Done.
    
-------------------------------------------------------------*/


struct y_struct_browser
{
    char* name;
    int chance; // Maybe better to call this "weight", as this really isn't a percentage.
    void* next; // Pointer to the next element in a single-linked list of browsers

    int max_connections_per_host;
    int max_connections;
    char* user_agent_string;
};

// For clarity purposes we hide the exact type of a browser here.
typedef struct y_struct_browser y_browser;


// Set up a list of these things.
#define MAX_BROWSER_LIST_LENGTH 1000 // To prevent loops caused by faulty data files or parameter settings ..
y_browser* y_browser_list_head = NULL;
int y_browser_list_chance_total = 0; // Cache rather than calculate on the fly.. 
                                     // Theoretically this should be per-list, however, rather than global.



void y_log_browser(const y_browser* browser)
{
    lr_log_message("y_browseremulation.c: browser: %s, chance %d, max_conn_per_host %d, max_conn %d, user agent string: %s",
                   browser->name,
                   browser->chance,
                   browser->max_connections_per_host,
                   browser->max_connections,
                   browser->user_agent_string);
}


void y_save_browser_to_parameters(const y_browser* browser)
{
    lr_save_string(browser->name, "browser_name");
    lr_save_int(browser->chance, "browser_chance");
    lr_save_int(browser->max_connections_per_host, "browser_max_connections_per_host");
    lr_save_int(browser->max_connections, "browser_max_connections");
    lr_save_string(browser->user_agent_string, "browser_user_agent_string");
}



void y_setup_browser_emulation()
{
    int i;
    y_browser* previous_browser = NULL;


    for(i=0; i < MAX_BROWSER_LIST_LENGTH; i++ )
    {
        y_browser* browser = (y_browser*) y_mem_alloc( sizeof browser[0] );

        browser->name = y_get_parameter_in_malloc_string("browser_name");
        if(strcmp(browser->name, "END") == 0)
        {
            lr_log_message("y_browseremulation.c: End of browser list initialisation");
            break;
        }

        // Fill out all remaining fields
        browser->next = NULL;
        browser->chance                   = atoi(y_get_parameter("browser_chance"));
        browser->max_connections_per_host = atoi(y_get_parameter("browser_max_connections_per_host"));
        browser->max_connections          = atoi(y_get_parameter("browser_max_connections"));
        browser->user_agent_string = y_get_parameter_in_malloc_string("browser_user_agent_string");

        //lr_log_message("y_browseremulation.c: Adding browser");
        //y_log_browser(browser);

        // Increment the global count of weights.
        y_browser_list_chance_total += browser->chance;
        lr_log_message("y_browseremulation.c: Adding weight: %d", y_browser_list_chance_total);

        // Add it to the list.
        if( y_browser_list_head == NULL )
        {
            y_browser_list_head = browser;
        }
        else
        {
            previous_browser->next = browser;
        }
        previous_browser = browser; // Replaces the previous tail element with the new one.

        // Get the next value for the new iteration.
        // This parameter should be set to "update each iteration", or this code will play havoc with it ..
        lr_advance_param("browser_name");
    };

    if( i == (MAX_BROWSER_LIST_LENGTH) )
    {
        lr_log_message("Too many browsers to fit in browser list struct, max list size = %d", MAX_BROWSER_LIST_LENGTH);
        lr_abort();
    }
}



// Given a list of a specified length, calculate what number all weights added together add up to.
// Do not call directly unless you have checked the list for sanity beforehand.
/*
int y_calculate_total_browser_chances(y_browser* browser_list_head)
{
    int total = 0;
    y_browser* browser; 

    for( browser = browser_list_head; browser->next != NULL; browser = browser->next)
    {
        //y_log_browser(browser);
        total += browser->chance;
    }
    lr_log_message("y_browser_emulation: Combined total of chances is: %d", total);
    return total;
}
*/


//
// Choose a browser profile from the browser list
// Returns a pointer to a randomly chosen struct browser or NULL in case of errors.
//
// Do not call directly unless you have checked the list for sanity beforehand.
y_browser* y_choose_browser_from_list(y_browser* browser_list_head )
{
    int i, lowerbound, cursor = 0;
    int max = y_browser_list_chance_total;
    long roll = y_rand() % max;
    y_browser* browser = NULL;

    // The upper bound of the rand() function is determined by the RAND_MAX constant.
    // RAND_MAX is hardcoded in loadrunner to a value of exactly 32767.
    // In fact, it doesn't even get #defined by loadrunner as as the C standard mandates.
    // (y_loadrunner_utils.c does that instead, now  ..)
    // 
    // y_rand() depends on rand() for it's output, so that cannot go above 32767.
    // Unfortunately, the list with weights that we use has numbers bigger than that. 
    // 
    // So we'll need to get a bit creative, and multiply the result of y_rand() with
    // the maximum (the total of the browser chances) divided by RAND_MAX to scale things up again.
    // 
    // Ugly? Yes. Necessary? Very ..
    // 
    // Update: y_rand() now returns a 31 bit number, giving it an upper bound of 2147483647.
    // That should reduce the need for this code by a bit ..
    // 
    // TODO: Fix y_profile.c as well, as it almost assuredly suffers from the same issue.

    //lr_log_message("max = %d, RAND_MAX = %d, roll %d", max, RAND_MAX, roll);
    if( RAND_MAX < max)
    {
        roll = roll * ((max / RAND_MAX));
    }
    lr_log_message("Roll: %d", roll);

    for( browser = browser_list_head; browser != NULL; browser = browser->next)
    {
        cursor += browser->chance;
        // lr_log_message("Chance cursor: %d for browser: %s", cursor, browser->name);

        if(roll <= cursor)
        {
            //lr_log_message("Chosen browser:");
            //y_log_browser(browser);
            return browser;
        }
    }
    // This code should be unreachable.
    lr_error_message("y_browseremulation.c: Roll result out of bounds: roll: %d, cursor: %d, max %d", roll, cursor, max);
    return browser;
}


y_browser* y_choose_browser()
{
    return y_choose_browser_from_list(y_browser_list_head);
}


void y_emulate_browser(const y_browser* browser)
{
    char str_max_connections[12];
    char str_max_connections_per_host[12];
    int max_connections;


    // Debugging purposes .
    lr_log_message("Emulating browser:");
    y_log_browser(browser);
    //browser->max_connections = 20000;
    //browser->max_connections_per_host = 20000;

    // This actually behaves pretty funky.. so let's not do this.
    //y_save_browser_to_parameters(browser);


    // Loadrunner doesn't accept values higher than 50 for this sockets option, so we'll just log it and set it to 50.
    max_connections = browser->max_connections;
    if( max_connections > 50 )
    {
        lr_log_message("y_browser_emulation.c: Loadrunner does not support using more than 50 browser connections. Using 50 connections instead of %d.", max_connections);
        max_connections = 50;
    }

    snprintf(str_max_connections,          sizeof str_max_connections,          "%d", max_connections);
    snprintf(str_max_connections_per_host, sizeof str_max_connections_per_host, "%d", browser->max_connections_per_host);

    // Now finally set the correct settings for the chosen browser:
    web_set_sockets_option("MAX_CONNECTIONS_PER_HOST", str_max_connections_per_host);
    web_set_sockets_option("MAX_TOTAL_CONNECTIONS",    str_max_connections);
    web_add_auto_header("User-Agent", browser->user_agent_string);
}


// --------------------------------------------------------------------------------------------------
#endif // _Y_BROWSER_EMULATION_C
