/*
*  Copyright (c) 2026 Michael Marley
*/

/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/poll.h>
#include <unistd.h>

#define NSEC_PER_SEC 1000000000
#define NSEC_PER_USEC 1000

#define INPUT_DEVICE_DIRECTORY_PATH "/dev/input"
#define EVENT_DEVICE_FILENAME_PREFIX "event"

#define BRIGHTNESS_FILE_PATH "/sys/class/leds/chromeos::kbd_backlight/brightness"
#define DEFAULT_TIMEOUT_SEC 10
#define DEFAULT_BRIGHTNESS 30
#define DEFAULT_FADE_DURATION_USEC 100000

// How long to wait after nudging the brightness before checking whether the firmware accepted it
#define AUTO_PROBE_DELAY_NSEC 200000000
#define AUTO_PROBE_MAX_ATTEMPTS 5
#define AUTO_PROBE_REQUIRED_IGNORES 2

// SD_LISTEN_FDS_START, but we don't want to link to systemd
#define BRIGHTNESS_FD 3

// Maximum number of input devices to watch
#define MAX_DEVICE_COUNT 8

// This must be set exactly to the number of elements in the array or else a segfault will occur
#define SEARCH_DEVICE_COUNT 1
static char const *const restrict search_devices[SEARCH_DEVICE_COUNT]={
	"AT Translated Set 2 keyboard",
};

// Any device whose name ends with this is also watched, since the touchpad model differs between Framework laptops
#define TOUCHPAD_NAME_SUFFIX " Touchpad"

static struct option const long_options[]={
	{"brightness",required_argument,0,'b'},
	{"fadeduration",required_argument,0,'f'},
	{"timeout",required_argument,0,'t'},
	{"levels",required_argument,0,'l'},
	{"help",no_argument,0,'h'},
	{},
};

static pthread_cond_t fader_cond;
static pthread_mutex_t fader_mutex;
static FILE *brightness_file;
static int fade_duration_nsec=DEFAULT_FADE_DURATION_USEC*NSEC_PER_USEC;
static int desired_brightness=0;
static bool fade_in_progress=false;

// Set while the firmware's auto brightness mode (one of the Fn+Space steps) is active; the firmware ignores writes then, so none are made
static _Atomic bool firmware_auto=false;
static int fader_target_brightness=0;

static bool exit_requested=false;
static bool is_daemon;

static void log_write(int const priority,char const *const restrict format,...){
	va_list args;
	va_start(args,format);
	if(is_daemon){
		vsyslog(priority,format,args);
	}else{
		FILE *target=priority<LOG_WARNING?stderr:stdout;
		vfprintf(target,format,args);
		fprintf(target,"\n");
	}
	va_end(args);
}

static void exit_handler(__attribute__((unused)) int const signal_number){
	exit_requested=true;
}

static int is_event_device(struct dirent const *const restrict directory_entry){
	return strncmp(EVENT_DEVICE_FILENAME_PREFIX,directory_entry->d_name,5)==0;
}

static bool string_in_array(char const *const restrict string,char const *const restrict array[],int const array_length){
	for(int array_index=0;array_index<array_length;array_index++){
		if(!strcmp(string,array[array_index])){
			return true;
		}
	}
	return false;
}

static bool string_ends_with(char const *const restrict string,char const *const restrict suffix){
	size_t const string_length=strlen(string);
	size_t const suffix_length=strlen(suffix);
	return string_length>=suffix_length&&!strcmp(string+string_length-suffix_length,suffix);
}

static int get_input_fds(struct pollfd *const restrict pollfds){
	struct dirent **device_name_list;
	int const device_count=scandir(INPUT_DEVICE_DIRECTORY_PATH,&device_name_list,is_event_device,alphasort);
	int const clock_type=CLOCK_MONOTONIC;
	
	int found_device_count=0;
	for(int device_index=0;device_index<device_count;device_index++){
		char device_filename[4096];
		char device_name[256]={};

		snprintf(device_filename,sizeof(device_filename),"%s/%s",INPUT_DEVICE_DIRECTORY_PATH,device_name_list[device_index]->d_name);
		free(device_name_list[device_index]);
		if(found_device_count>=MAX_DEVICE_COUNT){
			continue;
		}
		int const fd=open(device_filename,O_RDONLY);
		if(fd<0){
			continue;
		}
		ioctl(fd,EVIOCGNAME(sizeof(device_name)-1),device_name);

		if(string_in_array(device_name,search_devices,SEARCH_DEVICE_COUNT)||string_ends_with(device_name,TOUCHPAD_NAME_SUFFIX)){
			ioctl(fd,EVIOCSCLOCKID,&clock_type);
			log_write(LOG_INFO,"Using device %s:\t%s",device_filename,device_name);
			pollfds[found_device_count].fd=fd;
			pollfds[found_device_count].events=POLLIN;
			found_device_count++;
		}else{
			close(fd);
		}
	}
	if(device_count>=0){
		free(device_name_list);
	}

	return found_device_count;
}

static void set_brightness(int const brightness){
	if(firmware_auto){
		return;
	}
	fprintf(brightness_file,"%d",brightness);
}

// Read the brightness actually applied right now, which may have been changed by Fn+Space or the desktop environment
static int get_brightness(){
	FILE *const file=fopen(BRIGHTNESS_FILE_PATH,"r");
	if(!file){
		return -1;
	}
	int brightness;
	int const scan_result=fscanf(file,"%d",&brightness);
	fclose(file);
	return scan_result==1?brightness:-1;
}

// Check whether the firmware's auto brightness mode is active by nudging the brightness by 1% and seeing whether it sticks
// In auto mode the firmware ignores writes, so this is invisible either way
// The firmware also fades between Fn+Space steps on its own and overrides writes while doing so, so retry if the level is moving and only report auto mode after repeated ignored nudges
static bool firmware_auto_active(int *const restrict settled_brightness){
	struct timespec const probe_delay={.tv_sec=0,.tv_nsec=AUTO_PROBE_DELAY_NSEC};
	int ignored_count=0;
	for(int attempt=0;attempt<AUTO_PROBE_MAX_ATTEMPTS;attempt++){
		int const before_brightness=get_brightness();
		*settled_brightness=before_brightness;
		if(before_brightness<=0){
			return false;
		}
		int const probe_brightness=before_brightness>1?before_brightness-1:before_brightness+1;
		fprintf(brightness_file,"%d",probe_brightness);
		nanosleep(&probe_delay,NULL);
		int const after_brightness=get_brightness();
		if(after_brightness==probe_brightness){
			// The nudge stuck, so the firmware accepts writes; put the level back and give the firmware time to apply it before anything reads it
			fprintf(brightness_file,"%d",before_brightness);
			nanosleep(&probe_delay,NULL);
			return false;
		}else if(after_brightness==before_brightness&&++ignored_count>=AUTO_PROBE_REQUIRED_IGNORES){
			return true;
		}
		// Otherwise the level changed by itself (a firmware fade is in progress), so try again
	}
	return false;
}

static void start_fade(int const brightness){
	pthread_mutex_lock(&fader_mutex);
	desired_brightness=brightness;
	pthread_cond_signal(&fader_cond);
	pthread_mutex_unlock(&fader_mutex);
}

static bool is_fading(){
	pthread_mutex_lock(&fader_mutex);
	// A fade is pending if the fader hasn't picked up the latest desired_brightness yet
	bool const result=fade_in_progress||fader_target_brightness!=desired_brightness;
	pthread_mutex_unlock(&fader_mutex);
	return result;
}

static int string_to_int(int *const restrict result,int const min,int const max,char const *const restrict string){
	errno=0;
	long const long_value=strtol(string,NULL,10);
	if(errno!=0){
		return errno;
	}else if(long_value>=min&&long_value<=max){
		*result=(int)long_value;
		return EXIT_SUCCESS;
	}
	return EXIT_FAILURE;
}

// Remapping of firmware brightness levels (e.g. the Fn+Space steps) to levels of the user's choosing
#define MAX_LEVEL_MAP_COUNT 8
static int level_map_from[MAX_LEVEL_MAP_COUNT];
static int level_map_to[MAX_LEVEL_MAP_COUNT];
static int level_map_count=0;

// Parse a list like "20=5,75=40" into the level map
static int parse_level_map(char const *const restrict string){
	char buffer[256];
	snprintf(buffer,sizeof(buffer),"%s",string);
	char *save_pointer;
	for(char *token=strtok_r(buffer,",",&save_pointer);token;token=strtok_r(NULL,",",&save_pointer)){
		int from;
		int to;
		char extra;
		if(level_map_count>=MAX_LEVEL_MAP_COUNT||sscanf(token,"%d=%d%c",&from,&to,&extra)!=2||from<1||from>100||to<1||to>100){
			return EXIT_FAILURE;
		}
		level_map_from[level_map_count]=from;
		level_map_to[level_map_count]=to;
		level_map_count++;
	}
	return level_map_count>0?EXIT_SUCCESS:EXIT_FAILURE;
}

static int map_level(int const brightness){
	for(int map_index=0;map_index<level_map_count;map_index++){
		if(level_map_from[map_index]==brightness){
			return level_map_to[map_index];
		}
	}
	return brightness;
}

static bool timespec_gt(struct timespec const ts1,struct timespec const ts2){
	return (ts1.tv_sec>ts2.tv_sec)||(ts1.tv_sec==ts2.tv_sec&&ts1.tv_nsec>ts2.tv_nsec);
}

static int usage(){
	log_write(LOG_NOTICE,"Usage: keylightb [--brightness <brightness>] [--fadeduration <fadeduration>] [--timeout <timeout>] [--levels <from>=<to>,...]");
	log_write(LOG_NOTICE,"keylightb - automatic keyboard backlight daemon for Framework laptops");
	log_write(LOG_NOTICE,"Options:");
	log_write(LOG_NOTICE,"  --brightness\t\tbrightness level when active (1-100) [default=%d]",DEFAULT_BRIGHTNESS);
	log_write(LOG_NOTICE,"  --fadeduration\tfade time in microseconds (1-1000000) [default=%d]",DEFAULT_FADE_DURATION_USEC);
	log_write(LOG_NOTICE,"  --timeout\t\tactivity timeout in seconds (1-%d) [default=%d]",INT_MAX,DEFAULT_TIMEOUT_SEC);
	log_write(LOG_NOTICE,"  --levels\t\tuse different levels for Fn+Space steps, e.g. 20=5,75=40 (1-100)");
	log_write(LOG_NOTICE,"  --help\t\tdisplay usage information");
	return EXIT_FAILURE;
}

void *fader(void*){
	pthread_mutex_lock(&fader_mutex);
	
	int current_brightness=0;
	int local_desired_brightness=0;
	int fade_step_nsec=0;
	struct timespec next_fade_step_time={};
	
	int wait_result=0;
	while(true){
		if(wait_result==ETIMEDOUT){
			// If a fade step is due, calculate and set the brightness
			fade_step:
			current_brightness+=current_brightness>local_desired_brightness?-1:1;
			set_brightness(current_brightness);
			
			// Either wait to be signaled for the next fade or calculate next_fade_step_time as appropriate
			if(current_brightness==local_desired_brightness){
				fade_in_progress=false;
				goto fade_start_wait;
			}else{
				next_fade_step_time.tv_nsec+=fade_step_nsec;
				if(next_fade_step_time.tv_nsec>=NSEC_PER_SEC){
					next_fade_step_time.tv_sec++;
					next_fade_step_time.tv_nsec-=NSEC_PER_SEC;
				}
				goto fade_step_wait;
			}
		}else if(local_desired_brightness!=desired_brightness){
			// If the fade step is not due and desired_brightness has changed, calculate the start time and interval
			clock_gettime(CLOCK_MONOTONIC,&next_fade_step_time);
			local_desired_brightness=desired_brightness;
			fader_target_brightness=local_desired_brightness;

			// Fade from the brightness actually applied, in case Fn+Space or the desktop environment changed it
			int const actual_brightness=get_brightness();
			if(actual_brightness>=0){
				current_brightness=actual_brightness;
			}
			if(current_brightness==local_desired_brightness){
				fade_in_progress=false;
				goto fade_start_wait;
			}

			fade_in_progress=true;
			fade_step_nsec=fade_duration_nsec/abs(current_brightness-local_desired_brightness);
			goto fade_step;
		}else if(current_brightness==local_desired_brightness){
			// Handle spurious wakeups while not in a fade, wait until signal
			fade_start_wait:
			wait_result=pthread_cond_wait(&fader_cond,&fader_mutex);
		}else{
			// Handle spurious wakeups while in a fade, wait until next_fade_step_time (or signal)
			fade_step_wait:
			wait_result=pthread_cond_timedwait(&fader_cond,&fader_mutex,&next_fade_step_time);
		}
	}
}

int main(int const argc,char *const *const argv){
	is_daemon=!isatty(STDOUT_FILENO);
	
	int timeout_sec=DEFAULT_TIMEOUT_SEC;
	int configured_brightness=DEFAULT_BRIGHTNESS;

	// Set when the backlight is manually turned off while active, cleared when it is manually turned back on
	bool disabled=false;

	int option;
	while((option=getopt_long(argc,argv,"",long_options,NULL))!=EOF){
		switch(option){
			case -1:
			case 0:
				break;
			case 'b':
				if(string_to_int(&configured_brightness,1,100,optarg)){
					return usage();
				}
				break;
			case 'f':
				if(string_to_int(&fade_duration_nsec,1,1000000,optarg)){
					return usage();
				}
				fade_duration_nsec*=NSEC_PER_USEC;
				break;
			case 't':
				if(string_to_int(&timeout_sec,1,INT_MAX,optarg)){
					return usage();
				}
				break;
			case 'l':
				if(parse_level_map(optarg)){
					return usage();
				}
				break;
			case 'h':
			default:
				return usage();
		}
	}
	
	// Try to open the systemd-provided brightness file
	if(!(brightness_file=fdopen(BRIGHTNESS_FD,"w"))){
		// If that didn't work, try to open it ourselves
		if(!(brightness_file=fopen(BRIGHTNESS_FILE_PATH,"w"))){
			log_write(LOG_ERR,"Failed to open backlight device!  Check permissions and ensure you are running Linux kernel 6.11 or later.");
			return EXIT_FAILURE;
		}
	}
	setvbuf(brightness_file,NULL,_IONBF,0);
	
	struct pollfd found_device_pollfds[MAX_DEVICE_COUNT];
	int const found_device_count=get_input_fds(found_device_pollfds);
	if(found_device_count<1){
		log_write(LOG_ERR,"No matching input devices found!  Check permissions.");
		return EXIT_FAILURE;
	}
	
	log_write(LOG_INFO,"Backlight timeout: %d second%s",timeout_sec,timeout_sec!=1?"s":"");
	log_write(LOG_INFO,"Brightness level: %d%%",configured_brightness);
	log_write(LOG_INFO,"Fade duration: %d microsecond%s",fade_duration_nsec/NSEC_PER_USEC,fade_duration_nsec/NSEC_PER_USEC!=1?"s":"");
	for(int map_index=0;map_index<level_map_count;map_index++){
		log_write(LOG_INFO,"Level %d%% is used as %d%%",level_map_from[map_index],level_map_to[map_index]);
	}

	pthread_condattr_t condattr;
	pthread_condattr_init(&condattr);
	pthread_condattr_setclock(&condattr,CLOCK_MONOTONIC);
	
	pthread_cond_init(&fader_cond,&condattr);
	pthread_mutex_init(&fader_mutex,NULL);
	pthread_t fader_thread;
	pthread_create(&fader_thread,NULL,fader,NULL);
	
	// Set up the exit handler
	struct sigaction exit_action;
	exit_action.sa_handler=exit_handler;
	sigaction(SIGINT,&exit_action,NULL);
	sigaction(SIGTERM,&exit_action,NULL);
	
	set_brightness(0);
	
	struct timespec off_time={};
	while(true){
		if(exit_requested){
			break;
		}
		
		// If the backlight is on, we just woke up from sleeping until off_time at the bottom of the loop, so return immediately and process any input from that time period
		// If the backlight is off, sleep until new input occurs
		int poll_result=poll(found_device_pollfds,found_device_count,desired_brightness==configured_brightness?0:-1);
		if(poll_result>0){
			for(int device_index=0;device_index<found_device_count;device_index++){
				if(found_device_pollfds[device_index].revents&POLLIN){
					struct input_event input_events[512];
					int read_bytes=read(found_device_pollfds[device_index].fd,input_events,sizeof(input_events));
					if(read_bytes==-1&&errno!=EINTR){
						log_write(LOG_ERR,"Read failure‽");
						return EXIT_FAILURE;
					}
					
					// If more than one event was returned, the most recent event will be EV_SYN SYN_REPORT (unless events were dropped), so in that case start at the second-to-last and go backward so we see the actual event(s)…
					int event_count=read_bytes/sizeof(struct input_event);
					for(int event_index=event_count>=2?event_count-2:0;event_index>=0;event_index--){
						// Looking for only interesting events (EV_MSC, EV_KEY, EV_ABS, and EV_SYN SYN_DROPPED (which may appear if the buffer overflows at just the wrong time))…
						if(input_events[event_index].type==EV_MSC||input_events[event_index].type==EV_KEY||input_events[event_index].type==EV_ABS||(input_events[event_index].type==EV_SYN&&input_events[event_index].code==SYN_DROPPED)){
							// If one is found, calculate candidate_off_time and update off_time if off_time is older
							// This is necessary to ensure that older events from a latter device don't overwrite newer events from a former device
							struct timespec candidate_off_time;
							candidate_off_time.tv_sec=input_events[event_index].input_event_sec+timeout_sec;
							candidate_off_time.tv_nsec=input_events[event_index].input_event_usec*NSEC_PER_USEC;
							if(timespec_gt(candidate_off_time,off_time)){
								off_time=candidate_off_time;
							}
							break;
						}
					}
				}
			}
		}else if(poll_result==-1&&errno!=EINTR){
			log_write(LOG_ERR,"Poll failure‽");
			return EXIT_FAILURE;
		}
		
		struct timespec current_time;
		clock_gettime(CLOCK_MONOTONIC,&current_time);
		if(timespec_gt(off_time,current_time)){
			// If the off_time is in the future
			if(desired_brightness==0){
				if(firmware_auto){
					int settled_brightness;
					// Fn+Space cycles from auto to off, so a reading of 0 means auto mode was switched off
					if(get_brightness()==0){
						log_write(LOG_INFO,"Firmware auto brightness switched off, staying off until it is turned back on");
						firmware_auto=false;
						disabled=true;
					}else if(!firmware_auto_active(&settled_brightness)){
						// Recheck on each burst of activity, in case auto mode was misdetected or a fixed level was selected
						log_write(LOG_INFO,"Firmware accepts brightness changes again, resuming");
						firmware_auto=false;
					}
				}
				if(!firmware_auto&&!is_fading()){
					// And the backlight is currently off, check whether it was turned on while idle (e.g. Fn+Space or the desktop environment restoring it)
					int const actual_brightness=get_brightness();
					if(actual_brightness>0){
						if(disabled){
							log_write(LOG_INFO,"Backlight turned back on manually, resuming");
							disabled=false;
						}
						int const new_brightness=map_level(actual_brightness);
						if(new_brightness!=configured_brightness){
							log_write(LOG_INFO,"Brightness level changed to %d%%",new_brightness);
							configured_brightness=new_brightness;
						}
					}
				}

				// Turn it on, unless it was manually turned off or the firmware is in control
				if(!disabled&&!firmware_auto){
					log_write(LOG_DEBUG,"Turning backlight on");
					start_fade(configured_brightness);
				}
			}

			// Sleep until off_time
			clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&off_time,NULL);
		}else if(desired_brightness==configured_brightness){
			// Otherwise, check whether the brightness was changed while active, then turn it off
			if(!is_fading()){
				int const actual_brightness=get_brightness();
				if(actual_brightness==0){
					log_write(LOG_INFO,"Backlight turned off manually, staying off until it is turned back on");
					disabled=true;
				}else if(actual_brightness>0){
					int settled_brightness=actual_brightness;
					if(firmware_auto_active(&settled_brightness)){
						// Don't fade out (the firmware would undo it, causing a flash); writes are suppressed until auto mode is switched off
						log_write(LOG_INFO,"Firmware auto brightness is active, leaving the backlight to it until it is switched off with Fn+Space");
						firmware_auto=true;
					}else if(settled_brightness>0&&map_level(settled_brightness)!=configured_brightness){
						configured_brightness=map_level(settled_brightness);
						log_write(LOG_INFO,"Brightness level changed to %d%%",configured_brightness);
					}
				}
			}
			log_write(LOG_DEBUG,"Turning backlight off");
			start_fade(0);
		}
	}
	
	pthread_cancel(fader_thread);
	pthread_join(fader_thread,NULL);
	set_brightness(0);
	
	return EXIT_SUCCESS;
}
