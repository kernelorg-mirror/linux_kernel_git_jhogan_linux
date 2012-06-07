/* ir-debug-decoder.c - print raw IR waveform
 *
 * Copyright (C) 2012-2014 by James Hogan <james.hogan@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/export.h>
#include <linux/timer.h>
#include "rc-core-priv.h"

/**
 * struct waveform_layout - Describes ascii-art waveform layout.
 * @granularity:	Time duration of each non-edge character in uS.
 * @maxlen:		Maximum IR event length in uS.
 */
struct waveform_layout {
	int granularity;
	int maxlen;
};

/**
 * ir_debug_wave_line() - output a line of waveform.
 * @w:		Waveform layout information.
 * @style:	6 characters:
 * 		[0]: Rising edge.
 * 		[1]: First level high.
 * 		[2]: Further level highs.
 * 		[3]: Dashed level high.
 * 		[4]: Falling edge.
 * 		[5]: Level low.
 * 		[6]: Dashed level low.
 */
static void ir_debug_wave_line(struct debug_dec *data,
			       const struct waveform_layout *w,
			       const char *style)
{
	unsigned int i, j;
	int duration;
	char chr[2];

	printk("%c", data->events[0].pulse ? style[0] : style[4]);
	for (i = 0; i < data->num_events; ++i) {
		duration = TO_US(data->events[i].duration) + w->granularity / 2;
		chr[0] = data->events[i].pulse ? style[1] : style[5];
		chr[1] = data->events[i].pulse ? style[2] : style[5];
		if (duration > w->maxlen) {
			duration = w->maxlen;
			chr[0] = chr[1] = data->events[i].pulse ? style[3] : style[6];
		}
		j = 0;
		for (; duration > w->granularity; duration -= w->granularity) {
			printk("%c", chr[j]);
			j = 1;
		}
		printk("%c", data->events[i].pulse ? style[4] : style[0]);
	}
	printk("\n");
}

static void ir_debug_timeout(unsigned long arg)
{
	struct rc_dev *dev = (void *)arg;
	struct debug_dec *data = &dev->raw->debug;
	int p, i, duration;
	struct waveform_layout w;
	unsigned int order;

	spin_lock(&data->lock);

	if (!data->num_events)
		goto unlock;
	/* calibrate */
	w.granularity = 1000;
	for (i = 0, order = 1; i < data->num_events; ++i) {
		duration = TO_US(data->events[i].duration);
		/* find smallest interval */
		if (duration < w.granularity)
			w.granularity = duration;
		/* find maximum order */
		while (duration > order*10)
			order *= 10;
	}
	w.maxlen = 10000; /* 10 ms */

	/* draw waveform */
	ir_debug_wave_line(data, &w, " __.   ");
	ir_debug_wave_line(data, &w, "|   |  ");
	ir_debug_wave_line(data, &w, "|   |_.");

	/* print intervals times in uS vertically */
	for (; order > 0; order /= 10) {
		for (i = 0; i < data->num_events; ++i) {
			duration = TO_US(data->events[i].duration) + w.granularity / 2;
			if (duration > w.maxlen)
				duration = w.maxlen;
			for (; duration > w.granularity; duration -= w.granularity)
				printk(" ");
			duration = TO_US(data->events[i].duration);
			if (duration < order)
				printk(" ");
			else
				printk("%c", '0' + ((duration / order) % 10));
		}
		printk("\n");
	}
	/* for each detected protocol decoder */
	for (p = 0; p < data->num_decoders; ++p) {
		/* print symbols assigned to raw events */
		for (i = 0; i < data->num_events; ++i) {
			duration = TO_US(data->events[i].duration) + w.granularity / 2;
			if (duration > w.maxlen)
				duration = w.maxlen;
			for (; duration > w.granularity; duration -= w.granularity)
				printk(" ");
			printk("%c", data->symbols[p][i]);
		}
		/* list protocols being decoded */
		printk(" %s\n", rc_protocol_name(data->decoder_protos[p]));
	}

	/* reset the buffer */
	data->num_events = 0;
	data->num_decoders = 0;

unlock:
	spin_unlock(&data->lock);
}

/**
 * ir_debug_symbol() - Report a symbol detected by a raw IR decoder.
 * @dev:	RC device.
 * @handler:	IR raw handler.
 * @symbol:	Single character representing the symbol to emit in debug.
 *
 * Raw IR decoders can use this function to report symbols detected in the IR,
 * which can then be printed on a waveform as part of debug output.
 * Common symbols to be used are:
 * - 'H':	Header symbol
 * - '0':	A binary zero symbol
 * - '1':	A binary one symbol
 * - 'E':	End of valid signal
 * - 'A'-'Z':	Other protocol specific symbols
 * - 'a'-'z':	Debug output (such as reporting state machine states)
 */
int ir_debug_symbol(struct rc_dev *dev, struct ir_raw_handler *handler,
		    char symbol)
{
	struct debug_dec *data = &dev->raw->debug;

	spin_lock_bh(&data->lock);
	if (data->num_events) {
		unsigned int p;
		for (p = 0; p < data->num_decoders; ++p)
			if (data->decoder_protos[p] == handler->protocols)
				break;
		if (p == data->num_decoders) {
			if (p == ARRAY_SIZE(data->decoder_protos))
				goto unlock;
			++data->num_decoders;
			data->decoder_protos[p] = handler->protocols;
		}
		data->symbols[p][data->num_events - 1] = symbol;
	}
unlock:
	spin_unlock_bh(&data->lock);
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(ir_debug_symbol);

/**
 * ir_debug_decode() - Report a raw event for debug processing.
 * @dev:	RC device.
 * @ev:		Raw IR event.
 *
 * This function is used by ir-raw.c to report a raw event to the debug system
 * prior to providing the event to any raw IR handlers. The raw data is stored
 * in a buffer and printed as a waveform after a sufficient gap with any symbols
 * generated by individual raw IR handlers.
 */
int ir_debug_decode(struct rc_dev *dev, struct ir_raw_event ev)
{
	struct debug_dec *data = &dev->raw->debug;

	/* one-time per-device initialisation */
	if (!data->end_timer.function) {
		setup_timer(&data->end_timer, ir_debug_timeout,
			    (unsigned long)dev);
		spin_lock_init(&data->lock);
	}

	if (!is_timing_event(ev))
		return 0;

	spin_lock_bh(&data->lock);
	if (data->num_events < ARRAY_SIZE(data->events)) {
		unsigned int p;
		data->events[data->num_events] = ev;
		for (p = 0; p < ARRAY_SIZE(data->decoder_protos); ++p)
			data->symbols[p][data->num_events] = ' ';
		barrier();
		++data->num_events;
		/* delay the handler another 200 ms */
		mod_timer(&data->end_timer,
			  jiffies + msecs_to_jiffies(200));
	}
	spin_unlock_bh(&data->lock);
	return -EINVAL;
}
