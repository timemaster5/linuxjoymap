/*
 * Joystick remapper for the input driver suite.
 * by Alexandre Hardy 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */


#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mapper.h"

#define MAX_SIGNALS		16
#define MAX_EVENTS		32

#define MSECS(t)	(1000 * ((t) / HZ) + 1000 * ((t) % HZ) / HZ)

#define TIMEOUT			20 //(50 times a second)

#undef press
#undef release

struct shift_map {
	struct program_button_remap *button_press[KEY_MAX+1];
	struct program_button_remap *button_release[KEY_MAX+1];
	struct program_axis_remap *axes[ABS_MAX+1];
};

struct mapping {
	int fd; /* /dev/input/event* file descriptor */
	__u16 vendor;
	__u16 product;
	int jsnum;
	int mapped;
	//two maps, one for shifted, and one for not shifted
	struct shift_map map[2];
};

static int shifted=0;
static __u16 shift_vendor;
static __u16 shift_product;
static __u16 shift_button;
struct mapping *events[MAX_EVENTS];

//install after creating joysticks and code joystick
//our joysticks are vendor 0xff and product 0x01 and must not be handled
//our mice are vendor 0xff and product 0x02 and must not be handled
//our kbd are vendor 0xff and product 0x03 and must not be handled
//the code joystick is 0xff and product 0x00 
//the code joystick must be handled by our code
void install_event_handlers() {
	int i, j;	
	char name[256];
	struct input_id id;
	for (i=0; i<=MAX_EVENTS; i++) {
		sprintf(name, "/dev/input/event%d", i);
		int fd=open(name, O_RDONLY|O_NONBLOCK);
		if (fd<0) {
			events[i]=NULL;
		} else {
			ioctl(fd, EVIOCGID, &id);
			if ((id.vendor==0x00ff)&&(id.product!=0x0000)) {
				close(fd);
				events[i]=NULL;
				continue;
			}
			char devname[256];
			memset(devname, 0, 256);
			ioctl(fd, EVIOCGNAME(255), devname);
			fprintf(stderr, "Found device %s (vendor=0x%x, product=0x%x)\n", devname, id.vendor, id.product);
			events[i]=(struct mapping *)malloc(sizeof(struct mapping));
			events[i]->fd=fd;
			events[i]->vendor=id.vendor;
			events[i]->product=id.product;
			events[i]->jsnum=-1;
			events[i]->mapped=0;

			for (j=0; j<KEY_MAX+1; j++) {
				events[i]->map[0].button_press[j]=NULL;
				events[i]->map[0].button_release[j]=NULL;
				events[i]->map[1].button_press[j]=NULL;
				events[i]->map[1].button_release[j]=NULL;
			}
			for (j=0; j<ABS_MAX+1; j++) {
				events[i]->map[0].axes[j]=NULL;
				events[i]->map[1].axes[j]=NULL;
			}
		}
	}
}


static void process_key(struct mapping *mapper, int key, int value);
static void process_axis(struct mapping *mapper, int axis, int value);
void poll_joystick_loop() {
	int i, j, n=0;
        struct pollfd polled_fds[MAX_EVENTS];
	struct mapping *poll_mapper[MAX_EVENTS];
	struct input_event ev[16];
	int rb;
	for (i=0; i<MAX_EVENTS; i++) {
		if (events[i] && (events[i]->mapped)) {
			poll_mapper[n]=events[i];
			polled_fds[n].fd=events[i]->fd;
			polled_fds[n++].events=POLLIN;
		}
	}

	poll(polled_fds, n, TIMEOUT);
	for (i=0; i<n; i++) {
		if (polled_fds[i].revents&POLLIN) {
			rb=1;
			while (rb>0) {
				rb=read(polled_fds[i].fd, ev, sizeof(struct input_event)*16);
				if (rb>0)
				for (j=0; j<(int)(rb/sizeof(struct input_event)); j++) {
					if (ev[j].type==EV_KEY) {
			                        if ((ev[j].code >= BTN_MISC) && (ev[j].value != 2)) 
							process_key(poll_mapper[i], ev[j].code, ev[j].value); 
					}
					if (ev[j].type==EV_ABS)
						process_axis(poll_mapper[i], ev[j].code, ev[j].value);
				}
			}
		}
	}
	program_run();
	repeat_mouse_move();
}

static int nsignals=0;
static int signals[MAX_SIGNALS];
static int sighead=0;
static int sigtail=0;
int no_signal(void) {
	return (nsignals==0);
}

int goto_next_signal(void) {
	int r=signals[sighead];
	nsignals--;
	sighead++;
	sighead=sighead%MAX_SIGNALS;
	return r;
}

void push_signal(int signal) {
	//silently drop signals if too many are sent
	if (nsignals>=MAX_SIGNALS) return;
	nsignals++;
	signals[sigtail]=signal;
	sigtail++;
	sigtail=sigtail%MAX_SIGNALS;
}

static void process_key(struct mapping *mapper, int key, int value) {
	int button=0;
	int i, j;
	struct program_button_remap **button_remap;

	if ((mapper->vendor==shift_vendor)||(mapper->product==shift_product)) {
		if (key==shift_button) {
			shifted=value;
			if (shifted!=0) shifted=1;
		}
	}

	if ((mapper->vendor!=0x00ff)||(mapper->product!=0x0000))
		code_notify_button(mapper->jsnum, key, value);
	if (value==1) button_remap=mapper->map[shifted].button_press;
	else if (value==0) button_remap=mapper->map[shifted].button_release;
	else return;
	if (button_remap==NULL) return;
	if (button_remap[key]==NULL) return;
	
	j=button_remap[key]->device&0x0F;
	switch (button_remap[key]->device&0xF0) {
		case DEVICE_JOYSTICK:
			if (button_remap[key]->type==TYPE_BUTTON) {
				for (i=0; (i<MAX_SEQUENCE) && (button_remap[key]->sequence[i]!=SEQUENCE_DONE); i++) {
					button=button_remap[key]->sequence[i]&KEYMASK;
					if (button_remap[key]->sequence[i]&RELEASEMASK) value=0;
					else value=1;
					press_joy_button(j, button, value);
					if ((button_remap[key]->flags&FLAG_AUTO_RELEASE)&&(value==1)) {
						press_joy_button(j, button, 0);
					}
				}
			} else if (button_remap[key]->type==TYPE_AXIS) {
				//it is an axis
				if (value==1) value=32767;
				if (button_remap[key]->flags&FLAG_INVERT) value=-value;
				set_joy_axis(j, button_remap[key]->sequence[0], value);		
			} 
			break;
		case DEVICE_MOUSE:
			if (button_remap[key]->type==TYPE_BUTTON) {
				for (i=0; (i<MAX_SEQUENCE) && (button_remap[key]->sequence[i]!=SEQUENCE_DONE); i++) {
					button=button_remap[key]->sequence[i]&KEYMASK;
					if (button_remap[key]->sequence[i]&RELEASEMASK) value=0;
					else value=1;
					press_mouse_button(button, value);
					if ((button_remap[key]->flags&FLAG_AUTO_RELEASE)&&(value==1)) {
						press_mouse_button(button, 0);
					}
				}
			} else if (button_remap[key]->type==TYPE_AXIS) {
				//it is an axis
				if (button_remap[key]->flags&FLAG_INVERT) value=-value;
				if (button_remap[key]->sequence[0]==ABS_X) 
					move_mouse_x(value);
				if (button_remap[key]->sequence[0]==ABS_Y) 
					move_mouse_y(value);
				if (button_remap[key]->sequence[0]==ABS_WHEEL) 
					move_mouse_wheel(value);
			} 
			break;
		case DEVICE_KBD:
			if (button_remap[key]->type==TYPE_BUTTON) {
				for (i=0; (i<MAX_SEQUENCE) && (button_remap[key]->sequence[i]!=SEQUENCE_DONE); i++) {
					button=button_remap[key]->sequence[i]&KEYMASK;
					if (button_remap[key]->sequence[i]&RELEASEMASK) value=0;
					else value=1;
					press_key(button, value);
					if ((button_remap[key]->flags&FLAG_AUTO_RELEASE)&&(value==1)) {
						press_key(button, 0);
					}
				}
			}	
			break;
	}
}

static void process_axis(struct mapping *mapper, int axis, int value) {
	int button=0;
	int j;
	struct program_axis_remap **axes_remap;

	if ((mapper->vendor!=0x00ff)||(mapper->product!=0x0000))
		code_notify_axis(mapper->jsnum, axis, value);
	axes_remap=mapper->map[shifted].axes;
	if (axes_remap==NULL) return;
	if (axes_remap[axis]==NULL) return;
	j=axes_remap[axis]->device&0x0F;
	switch (axes_remap[axis]->device&0xF0) {
		case DEVICE_JOYSTICK:
			if (axes_remap[axis]->type==TYPE_BUTTON) {
				//a joystick button
				if (value<0)
					button=axes_remap[axis]->minus&KEYMASK;
				else
					button=axes_remap[axis]->plus&KEYMASK;
				value=1;
				press_joy_button(j, button, 1);
				if (axes_remap[axis]->flags&FLAG_AUTO_RELEASE) {
					press_joy_button(j, button, 0);
				}
			} else if (axes_remap[axis]->type==TYPE_AXIS) {
				//it is an axis
				if (axes_remap[axis]->flags&FLAG_INVERT) value=-value;
				set_joy_axis(j, axes_remap[axis]->axis, value);		
			} 
			break;
		case DEVICE_MOUSE:
			if (axes_remap[axis]->type==TYPE_BUTTON) {
				//a joystick button
				if (value<0)
					button=axes_remap[axis]->minus&KEYMASK;
				else
					button=axes_remap[axis]->plus&KEYMASK;
				value=1;
				press_mouse_button(button, 1);
				if (axes_remap[axis]->flags&FLAG_AUTO_RELEASE) {
					press_mouse_button(button, 0);
				}
			} else if (axes_remap[axis]->type==TYPE_AXIS) {
				//it is an axis
				value-=127;
				value/=32;
				if (axes_remap[axis]->flags&FLAG_INVERT) value=-value;
				//if (value>0) value=1;
				//if (value<0) value=-1;
				if (axes_remap[axis]->axis==ABS_X) 
					move_mouse_x(value);
				if (axes_remap[axis]->axis==ABS_Y) 
					move_mouse_y(value);
				if (axes_remap[axis]->axis==ABS_WHEEL) 
					move_mouse_wheel(value);
			} 
			break;
		case DEVICE_KBD:
			if (axes_remap[axis]->type==TYPE_BUTTON) {
				//a joystick button
				if (value<0)
					button=axes_remap[axis]->minus&KEYMASK;
				else
					button=axes_remap[axis]->plus&KEYMASK;
				value=1;
				press_key(button, 1);
				if (axes_remap[axis]->flags&FLAG_AUTO_RELEASE) {
					press_key(button, 0);
				}
			}	
			break;
	}
}

void remap_axis(struct program_axis_remap *axis) {
	struct mapping *mapper;
	int i, shifted;

	if (axis->program!=PROGRAM_AXIS_REMAP) return;
	if (axis->srcaxis>ABS_MAX) return;
	
	mapper=NULL;
	for (i=0; i<MAX_EVENTS; i++) {
		if (events[i])
			if ((events[i]->vendor==axis->vendor)&&
			   (events[i]->product==axis->product))
				mapper=events[i];
	}
	
	if (mapper==NULL) return;
	mapper->mapped=1;
	ioctl(mapper->fd, EVIOCGRAB, 1);

	shifted=0;
	if (axis->flags&FLAG_SHIFT) shifted=1;
	mapper->map[shifted].axes[axis->srcaxis]=axis;
}

void remap_button(struct program_button_remap *btn) {
	struct mapping *mapper;
	int i, shifted;

	if (btn->program!=PROGRAM_BUTTON_REMAP) return;
	if (btn->srcbutton>KEY_MAX) return;
	
	mapper=NULL;
	for (i=0; i<MAX_EVENTS; i++) {
		if (events[i])
			if ((events[i]->vendor==btn->vendor)&&
			   (events[i]->product==btn->product))
				mapper=events[i];
	}
	
	if (mapper==NULL) return;
	mapper->mapped=1;
	ioctl(mapper->fd, EVIOCGRAB, 1);

	shifted=0;
	if (btn->flags&FLAG_SHIFT) shifted=1;

	if (btn->type==TYPE_SHIFT) {
		shift_vendor=btn->vendor;
		shift_product=btn->product;
		shift_button=btn->srcbutton;
	} else {
		if (btn->flags&FLAG_RELEASE)
			mapper->map[shifted].button_release[btn->srcbutton]=btn;
		else
			mapper->map[shifted].button_press[btn->srcbutton]=btn;
	}
}

void set_joystick_number(__u16 vendor, __u16 product, int device) {
	struct mapping *mapper;
	int i;
	mapper=NULL;
	for (i=0; i<MAX_EVENTS; i++) {
		if (events[i])
			if ((events[i]->vendor==vendor)&&
			   (events[i]->product==product))
				mapper=events[i];
	}
	
	if (mapper==NULL) return;
	mapper->mapped=1;
	mapper->jsnum=device;
}
