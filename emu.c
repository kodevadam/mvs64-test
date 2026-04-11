#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "emu.h"
#include "m68k.h"
#include "hw.h"
#include "video.h"
#include "roms.h"
#include "platform.h"

static int cpu_trace_count = 0;
void cpu_trace(unsigned int pc) {
	(void)cpu_trace_count;
	#ifndef N64
	if (cpu_trace_count == 0) {
		m68k_set_instr_hook_callback(NULL);
		return;
	}

	char inst[1024];
	m68k_disassemble(inst, pc, M68K_CPU_TYPE_68000);
	debugf("trace: %06x %-30s", pc, inst);

	if (strstr(inst, "A0")) debugf("A0=%08x ", m68k_get_reg(NULL, M68K_REG_A0));
	if (strstr(inst, "A1")) debugf("A1=%08x ", m68k_get_reg(NULL, M68K_REG_A1));
	if (strstr(inst, "A2")) debugf("A2=%08x ", m68k_get_reg(NULL, M68K_REG_A2));
	if (strstr(inst, "A3")) debugf("A3=%08x ", m68k_get_reg(NULL, M68K_REG_A3));
	if (strstr(inst, "A4")) debugf("A4=%08x ", m68k_get_reg(NULL, M68K_REG_A4));
	if (strstr(inst, "A5")) debugf("A5=%08x ", m68k_get_reg(NULL, M68K_REG_A5));
	if (strstr(inst, "A6")) debugf("A6=%08x ", m68k_get_reg(NULL, M68K_REG_A6));
	if (strstr(inst, "A7")) debugf("A7=%08x ", m68k_get_reg(NULL, M68K_REG_A7));
	if (strstr(inst, "D0")) debugf("D0=%08x ", m68k_get_reg(NULL, M68K_REG_D0));
	if (strstr(inst, "D1")) debugf("D1=%08x ", m68k_get_reg(NULL, M68K_REG_D1));
	if (strstr(inst, "D2")) debugf("D2=%08x ", m68k_get_reg(NULL, M68K_REG_D2));
	if (strstr(inst, "D3")) debugf("D3=%08x ", m68k_get_reg(NULL, M68K_REG_D3));
	if (strstr(inst, "D4")) debugf("D4=%08x ", m68k_get_reg(NULL, M68K_REG_D4));
	if (strstr(inst, "D5")) debugf("D5=%08x ", m68k_get_reg(NULL, M68K_REG_D5));
	if (strstr(inst, "D6")) debugf("D6=%08x ", m68k_get_reg(NULL, M68K_REG_D6));
	if (strstr(inst, "D7")) debugf("D7=%08x ", m68k_get_reg(NULL, M68K_REG_D7));

	debugf("\n");

	cpu_trace_count--;
	#endif
}

void cpu_start_trace(int cnt) {
	#ifndef N64
	m68k_set_instr_hook_callback(cpu_trace);
	#endif
	cpu_trace_count = cnt;
}

static int g_frame;
static uint64_t g_clock, g_clock_framebegin;
static uint64_t m68k_clock;
static EmuEvent events[MAX_EVENTS];
uint32_t profile_hw_io;
uint32_t profile_dma_load;

static uint64_t m68k_exec(uint64_t clock) {
	clock /= M68K_CLOCK_DIV;
	if (clock > m68k_clock) {
		int ncycles = (int)(clock - m68k_clock);
		int executed = m68k_execute(ncycles);
		m68k_clock += executed;
	}
	return m68k_clock * M68K_CLOCK_DIV;
}


// Return the next event that must be executed
static EmuEvent* next_event() {
    EmuEvent *e = NULL;
    for (int i=0;i<MAX_EVENTS;i++) {
        if (!events[i].cb) continue;
        if (!e || events[i].clock < e->clock) e=&events[i];
    }
    return e;
}

int emu_add_event(int64_t clock, EmuEventCb cb, void *cbarg) {
    for (int i=0;i<MAX_EVENTS;i++) {
        if (events[i].cb) continue;
        events[i].clock = clock;
        events[i].cb = cb;
        events[i].cbarg = cbarg;
        events[i].current = false;
        return i;
    }
    assert(0);
}

void emu_change_event(int event_id, int64_t newclock) {
	events[event_id].clock = newclock;
	if (events[event_id].current) {
		m68k_end_timeslice();
	}
}

int64_t emu_clock(void) {
	return g_clock + m68k_cycles_run() * M68K_CLOCK_DIV;
}

int64_t emu_clock_frame(void) {
	return emu_clock() - g_clock_framebegin;
}

void emu_cpu_reset(void) {
	m68k_pulse_reset();
}

uint32_t emu_pc(void) {
	return m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
}

void emu_cpu_irq(int irq, bool on) {
	m68k_set_virq(irq, on);
}


uint32_t emu_vblank_start(void* arg) {
	emu_cpu_irq(1, true);
	hw_vblank();
	return FRAME_CLOCK;
}

uint32_t render_time;

uint32_t emu_render(void *arg) {

	#ifdef N64
	if (CONFIG_FRAMESKIP_MODE == 2) {
		extern volatile int N64_FRAME;
		const int MAX_SKIP = 4;
		static int skip = 0;

		if (N64_FRAME > g_frame) {
			skip++;
			if (skip < MAX_SKIP) {
				;
				return FRAME_CLOCK;
			}
			;
			skip = 0;
			disable_interrupts();
			N64_FRAME = g_frame;
			enable_interrupts();
		}
	}
	#endif

	if (CONFIG_FRAMESKIP_MODE == 1) {
		if (g_frame % 3 != 0) {
			;
			return FRAME_CLOCK;
		}
	}

	#ifdef N64
	uint32_t t0 = TICKS_READ();
	#endif
	plat_beginframe();
	video_render();
	plat_endframe();

	rom_next_frame();

	#ifdef N64
	render_time = TICKS_DISTANCE(t0, TICKS_READ());
	#endif

	return FRAME_CLOCK;
}

void emu_run_frame(void) {
    uint64_t vsync = g_clock_framebegin + FRAME_CLOCK;
    EmuEvent *e;

    // Run all events that are scheduled before next vsync
    while ((e = next_event()) && (e->clock < vsync)) {
    	e->current = true;
        g_clock = m68k_exec(e->clock);
        e->current = false;

        // Call the event callback, and check if it must be repeated.
        if (g_clock >= e->clock) {    	
	        uint32_t repeat = e->cb(e->cbarg);
	        if (repeat != 0) e->clock += repeat;
	        else e->cb = NULL;
        }
    }

    while (g_clock < vsync)
    	g_clock = m68k_exec(vsync);

    g_frame++;
	g_clock_framebegin += FRAME_CLOCK;
}

int main(int argc, char *argv[]) {
	#ifndef N64
	if (argc < 2) {
		fprintf(stderr, "Usage:\n    mvs64 <romdir>\n");
		return 1;
	}
	#else 
	argc = 0; argv = NULL;
	#endif

	plat_init(44100, FPS);
	plat_enable_video(true);

	#ifdef N64
	rom_load("rom:/");
	#else
	rom_load(argv[1]);
	#endif

	m68k_init();
	{
		extern m68ki_cpu_core m68ki_cpu;
		m68ki_cpu.fetch_region = 0xFF; /* force first m68k_update_fetch_ptr to recalculate */
	}

	hw_init();
	g_clock = 0;

	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	m68k_pulse_reset();
	m68k_clock = 0;

	emu_add_event(LINE_CLOCK*24,  emu_render, NULL);
	emu_add_event(LINE_CLOCK*248, emu_vblank_start, NULL);

	#ifdef N64
	uint32_t fps_frame = 0;
	uint32_t fps_time = TICKS_READ();
	#endif
	while (1) {
		render_time = 0;
		profile_hw_io = 0;
		profile_dma_load = 0;
		#ifdef N64
		uint32_t t0 = TICKS_READ();
		#endif
		emu_run_frame();
		if (!plat_poll()) break;

		#ifdef N64
		uint32_t emu_time = TICKS_DISTANCE(t0, TICKS_READ());

		debugf("[PROFILE] cpu:%.2f%% io:%.2f%% draw:%.2f%% dma:%.2f%% PC:%06lx\n",
			(float)emu_time * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)profile_hw_io * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)render_time * 100.f / (float)(TICKS_PER_SECOND / 60),
			(float)profile_dma_load * 100.f / (float)(TICKS_PER_SECOND / 60),
			(uint32_t)m68k_get_reg(NULL, M68K_REG_PC));
		#endif

		rom_next_frame();
		#ifdef N64
		uint32_t curtime = TICKS_READ();
		if (TICKS_DISTANCE(fps_time, curtime) > TICKS_FROM_MS(1000)) {
			debugf("FPS: %.1f\n", (g_frame - fps_frame) * (float)TICKS_PER_SECOND / TICKS_DISTANCE(fps_time, curtime));
			fps_frame = g_frame;
			fps_time = curtime;
		}
		#endif
	}

	debugf("end\n");
	cpu_start_trace(1000);
	m68k_exec(g_clock+100);

	#ifndef N64
	FILE *f = fopen("vram.dump", "wb");
	fwrite(VIDEO_RAM, 1, sizeof(VIDEO_RAM), f);
	fclose(f);
	#endif

	plat_save_screenshot("screen.bmp");
}
