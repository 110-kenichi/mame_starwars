// license:BSD-3-Clause
// copyright-holders:Steve Baines, Frank Palazzolo
/***************************************************************************

    Atari Star Wars hardware

    driver by Steve Baines and Frank Palazzolo

    This file is Copyright Steve Baines.
    Modified by Frank Palazzolo for sound support

    Games supported:
        * Star Wars
        * The Empire Strikes Back
        * TomCat prototype on Star Wars hardware

    Known bugs:
        * the monitor "overdrive" effect is not simulated when you
          get hit by enemy fire

****************************************************************************

    Memory map (TBA)

***************************************************************************/

#include "emu.h"
#include "starwars.h"

#include "cpu/m6809/m6809.h"
#include "machine/74259.h"
#include "machine/adc0808.h"
#include "machine/watchdog.h"
#include "osdcore.h"
#include "video/avgdvg.h"
#include "video/vector.h"
#include "screen.h"
#include "speaker.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>

#if defined(_WIN32)
#include "../../osd/windows/winopts.h"
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif


#define MASTER_CLOCK (12.096_MHz_XTAL)
#define CLOCK_3KHZ   (MASTER_CLOCK / 4096)

class starwars_scope_openal_output
{
public:
	starwars_scope_openal_output(running_machine &machine, screen_device &screen)
		: m_machine(machine)
		, m_screen(screen)
	{
	}

	~starwars_scope_openal_output()
	{
		stop();
	}

	bool start()
	{
	#if defined(_WIN32)
		char const *const device_name = m_machine.options().value(WINOPTION_XY_SCOPE_DEVICE);
		char const *resolved_name = nullptr;

		if ((nullptr == device_name) || !device_name[0])
			return false;

		if (!m_openal.load())
		{
			osd_printf_error("Star Wars scope output: unable to load OpenAL32.dll\n");
			return false;
		}

		m_speed_scale = std::clamp(
				double(m_machine.options().float_value(WINOPTION_OSC_SPEED_FACTOR)),
				0.5,
				5.0);
		m_sample_rate = std::max(m_machine.options().sample_rate(), 44100);
		m_frame_samples = std::max(m_sample_rate / 240, 256);
		m_pcm.resize(std::size_t(m_frame_samples) * 2U);
		m_buffer_ids.resize(m_buffer_count);

		resolved_name = find_device(device_name);
		m_device = m_openal.alcOpenDevice(resolved_name ? resolved_name : device_name);
		if (!m_device && device_name[0])
			m_device = m_openal.alcOpenDevice(nullptr);
		if (!m_device)
		{
			osd_printf_error("Star Wars scope output: unable to open OpenAL device '%s'\n", device_name);
			stop();
			return false;
		}

		m_context = m_openal.alcCreateContext(m_device, nullptr);
		if (!m_context)
		{
			osd_printf_error("Star Wars scope output: unable to create OpenAL context\n");
			stop();
			return false;
		}

		if (!m_openal.alcMakeContextCurrent(m_context))
		{
			osd_printf_error("Star Wars scope output: unable to activate OpenAL context\n");
			stop();
			return false;
		}

		m_openal.alGenSources(1, &m_source_id);
		if (!m_source_id)
		{
			osd_printf_error("Star Wars scope output: unable to allocate OpenAL source\n");
			stop();
			return false;
		}

		m_openal.alGenBuffers(static_cast<ALsizei>(m_buffer_ids.size()), m_buffer_ids.data());
		for (ALuint buffer_id : m_buffer_ids)
		{
			if (!buffer_id)
			{
				osd_printf_error("Star Wars scope output: unable to allocate OpenAL buffers\n");
				stop();
				return false;
			}
			queue_buffer(buffer_id);
		}

		m_openal.alSourcePlay(m_source_id);

		m_started = true;
		return true;
#else
		return false;
#endif
	}

	void stop()
	{
	#if defined(_WIN32)
		m_started = false;

		if (m_source_id)
		{
			m_openal.alSourceStop(m_source_id);
			if (!m_buffer_ids.empty())
				m_openal.alDeleteBuffers(static_cast<ALsizei>(m_buffer_ids.size()), m_buffer_ids.data());
			m_openal.alDeleteSources(1, &m_source_id);
			m_source_id = 0;
		}

		m_buffer_ids.clear();

		if (m_context)
		{
			m_openal.alcMakeContextCurrent(nullptr);
			m_openal.alcDestroyContext(m_context);
			m_context = nullptr;
		}

		if (m_device)
		{
			m_openal.alcCloseDevice(m_device);
			m_device = nullptr;
		}

		m_openal.unload();
#endif
	}

	bool active() const
	{
		return m_started;
	}

	void frame_begin()
	{
		m_pending_points.clear();
	}

	void add_line(int x0, int y0, int x1, int y1, int intensity)
	{
		rectangle const &visarea = m_screen.visible_area();
		double const half_width = double(visarea.max_x - visarea.min_x) * 32768.0;
		double const half_height = double(visarea.max_y - visarea.min_y) * 32768.0;
		double const xcenter = double((visarea.max_x + visarea.min_x) / 2) * 65536.0;
		double const ycenter = double((visarea.max_y + visarea.min_y) / 2) * 65536.0;
		double brightness;

		if ((half_width <= 0.0) || (half_height <= 0.0) || (intensity <= 0))
			return;

		brightness = double(intensity) / 255.0;
		brightness *= brightness;

		m_pending_points.emplace_back(scope_point{ (double(x0) - xcenter) / half_width, -(double(y0) - ycenter) / half_height, brightness });
		m_pending_points.emplace_back(scope_point{ (double(x1) - xcenter) / half_width, -(double(y1) - ycenter) / half_height, brightness });
	}

	void frame_end()
	{
		m_next_points = m_pending_points;
		m_next_frame_ready = true;

		if (m_points.size() < 2U)
			activate_pending_frame();

		service_openal();
	}

private:
	struct ALCdevice_struct;
	struct ALCcontext_struct;
	using ALCdevice = ALCdevice_struct;
	using ALCcontext = ALCcontext_struct;
	using ALchar = char;
	using ALCchar = char;
	using ALenum = int;
	using ALCenum = int;
	using ALint = int;
	using ALsizei = int;
	using ALuint = unsigned int;
	using ALvoid = void;
	using ALCboolean = int;

	static constexpr ALenum AL_FORMAT_STEREO_FLOAT32 = 0x10011;
	static constexpr ALenum AL_SOURCE_STATE = 0x1010;
	static constexpr ALenum AL_STOPPED = 0x1014;
	static constexpr ALenum AL_BUFFERS_PROCESSED = 0x1016;
	static constexpr ALCenum ALC_DEVICE_SPECIFIER = 0x1005;
	static constexpr ALCenum ALC_ALL_DEVICES_SPECIFIER = 0x1013;

	struct openal_api
	{
		using alcOpenDevice_proc = ALCdevice *(*)(ALCchar const *);
		using alcCloseDevice_proc = ALCboolean (*)(ALCdevice *);
		using alcCreateContext_proc = ALCcontext *(*)(ALCdevice *, ALint const *);
		using alcDestroyContext_proc = void (*)(ALCcontext *);
		using alcMakeContextCurrent_proc = ALCboolean (*)(ALCcontext *);
		using alcGetString_proc = ALCchar const *(*)(ALCdevice *, ALCenum);
		using alcIsExtensionPresent_proc = ALCboolean (*)(ALCdevice *, ALCchar const *);
		using alGenSources_proc = void (*)(ALsizei, ALuint *);
		using alDeleteSources_proc = void (*)(ALsizei, ALuint const *);
		using alGenBuffers_proc = void (*)(ALsizei, ALuint *);
		using alDeleteBuffers_proc = void (*)(ALsizei, ALuint const *);
		using alBufferData_proc = void (*)(ALuint, ALenum, ALvoid const *, ALsizei, ALsizei);
		using alSourceQueueBuffers_proc = void (*)(ALuint, ALsizei, ALuint const *);
		using alSourceUnqueueBuffers_proc = void (*)(ALuint, ALsizei, ALuint *);
		using alSourcePlay_proc = void (*)(ALuint);
		using alSourceStop_proc = void (*)(ALuint);
		using alGetSourcei_proc = void (*)(ALuint, ALenum, ALint *);

		HMODULE module = nullptr;
		alcOpenDevice_proc alcOpenDevice = nullptr;
		alcCloseDevice_proc alcCloseDevice = nullptr;
		alcCreateContext_proc alcCreateContext = nullptr;
		alcDestroyContext_proc alcDestroyContext = nullptr;
		alcMakeContextCurrent_proc alcMakeContextCurrent = nullptr;
		alcGetString_proc alcGetString = nullptr;
		alcIsExtensionPresent_proc alcIsExtensionPresent = nullptr;
		alGenSources_proc alGenSources = nullptr;
		alDeleteSources_proc alDeleteSources = nullptr;
		alGenBuffers_proc alGenBuffers = nullptr;
		alDeleteBuffers_proc alDeleteBuffers = nullptr;
		alBufferData_proc alBufferData = nullptr;
		alSourceQueueBuffers_proc alSourceQueueBuffers = nullptr;
		alSourceUnqueueBuffers_proc alSourceUnqueueBuffers = nullptr;
		alSourcePlay_proc alSourcePlay = nullptr;
		alSourceStop_proc alSourceStop = nullptr;
		alGetSourcei_proc alGetSourcei = nullptr;

		bool load()
		{
			if (module)
				return true;

			module = LoadLibraryA("OpenAL32.dll");
			if (!module)
				return false;

			return load_symbol(alcOpenDevice, "alcOpenDevice")
				&& load_symbol(alcCloseDevice, "alcCloseDevice")
				&& load_symbol(alcCreateContext, "alcCreateContext")
				&& load_symbol(alcDestroyContext, "alcDestroyContext")
				&& load_symbol(alcMakeContextCurrent, "alcMakeContextCurrent")
				&& load_symbol(alcGetString, "alcGetString")
				&& load_symbol(alcIsExtensionPresent, "alcIsExtensionPresent")
				&& load_symbol(alGenSources, "alGenSources")
				&& load_symbol(alDeleteSources, "alDeleteSources")
				&& load_symbol(alGenBuffers, "alGenBuffers")
				&& load_symbol(alDeleteBuffers, "alDeleteBuffers")
				&& load_symbol(alBufferData, "alBufferData")
				&& load_symbol(alSourceQueueBuffers, "alSourceQueueBuffers")
				&& load_symbol(alSourceUnqueueBuffers, "alSourceUnqueueBuffers")
				&& load_symbol(alSourcePlay, "alSourcePlay")
				&& load_symbol(alSourceStop, "alSourceStop")
				&& load_symbol(alGetSourcei, "alGetSourcei");
		}

		void unload()
		{
			if (module)
				FreeLibrary(module);
			module = nullptr;
			alcOpenDevice = nullptr;
			alcCloseDevice = nullptr;
			alcCreateContext = nullptr;
			alcDestroyContext = nullptr;
			alcMakeContextCurrent = nullptr;
			alcGetString = nullptr;
			alcIsExtensionPresent = nullptr;
			alGenSources = nullptr;
			alDeleteSources = nullptr;
			alGenBuffers = nullptr;
			alDeleteBuffers = nullptr;
			alBufferData = nullptr;
			alSourceQueueBuffers = nullptr;
			alSourceUnqueueBuffers = nullptr;
			alSourcePlay = nullptr;
			alSourceStop = nullptr;
			alGetSourcei = nullptr;
		}

	private:
		template <typename T> bool load_symbol(T &target, char const *name)
		{
			target = reinterpret_cast<T>(GetProcAddress(module, name));
			if (!target)
			{
				unload();
				return false;
			}
			return true;
		}
	};

	struct scope_point
	{
		double x;
		double y;
		double intensity;
	};

	struct clipped_segment
	{
		scope_point a;
		scope_point b;
		bool valid;
	};

	static bool char_equal_ci(char left, char right)
	{
		return std::tolower((unsigned char)left) == std::tolower((unsigned char)right);
	}

	static bool string_equal_ci(std::string const &left, std::string const &right)
	{
		return (left.size() == right.size())
			&& std::equal(left.begin(), left.end(), right.begin(), char_equal_ci);
	}

	static bool string_contains_ci(std::string const &haystack, std::string const &needle)
	{
		if (needle.empty())
			return true;

		auto match = [&needle] (char lhs, char rhs) { return char_equal_ci(lhs, rhs); };
		auto const it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), match);
		return haystack.end() != it;
	}

	char const *find_device(char const *requested_name)
	{
		ALCchar const *devices;
		ALCchar const *device_name;
		ALCenum specifier = ALC_DEVICE_SPECIFIER;

		if ((nullptr == requested_name) || !requested_name[0])
			return nullptr;

		if (m_openal.alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT"))
			specifier = ALC_ALL_DEVICES_SPECIFIER;

		devices = m_openal.alcGetString(nullptr, specifier);
		if (!devices || !devices[0])
			return nullptr;

		for (device_name = devices; device_name[0] != 0; device_name += std::strlen(device_name) + 1)
		{
			if (string_equal_ci(device_name, requested_name))
				return device_name;
		}

		for (device_name = devices; device_name[0] != 0; device_name += std::strlen(device_name) + 1)
		{
			if (string_contains_ci(device_name, requested_name))
				return device_name;
		}

		return nullptr;
	}

	static bool inside(scope_point const &point)
	{
		return (-1.0 <= point.x) && (1.0 >= point.x) && (-1.0 <= point.y) && (1.0 >= point.y);
	}

	static double distance_sq(scope_point const &a, scope_point const &b)
	{
		double const dx = a.x - b.x;
		double const dy = a.y - b.y;
		return (dx * dx) + (dy * dy);
	}

	static bool intersect(scope_point const &p0, scope_point const &p1, double bound, bool is_x, scope_point &out_point)
	{
		double const delta = is_x ? (p1.x - p0.x) : (p1.y - p0.y);
		double t;

		if (0.0 == delta)
			return false;

		t = is_x ? ((bound - p0.x) / delta) : ((bound - p0.y) / delta);
		if ((0.0 > t) || (1.0 < t))
			return false;

		out_point.x = is_x ? bound : (p0.x + ((p1.x - p0.x) * t));
		out_point.y = is_x ? (p0.y + ((p1.y - p0.y) * t)) : bound;
		out_point.intensity = p0.intensity + ((p1.intensity - p0.intensity) * t);
		return true;
	}

	static std::size_t nearest_intersection(std::vector<scope_point> const &points, scope_point const &reference)
	{
		std::size_t best = 0;
		double best_distance = distance_sq(points[0], reference);

		for (std::size_t i = 1; i < points.size(); ++i)
		{
			double const current = distance_sq(points[i], reference);
			if (current < best_distance)
			{
				best = i;
				best_distance = current;
			}
		}

		return best;
	}

	static clipped_segment clip_segment(scope_point const &p0, scope_point const &p1)
	{
		static double const bounds[4] = { -1.0, 1.0, -1.0, 1.0 };
		static bool const bound_is_x[4] = { true, true, false, false };
		clipped_segment result{ scope_point{}, scope_point{}, false };
		bool const p0_inside = inside(p0);
		bool const p1_inside = inside(p1);
		std::vector<scope_point> intersections;

		for (int i = 0; i < 4; ++i)
		{
			scope_point point;
			if (intersect(p0, p1, bounds[i], bound_is_x[i], point))
				intersections.push_back(point);
		}

		if (p0_inside && p1_inside)
			return clipped_segment{ p0, p1, true };

		if (!p0_inside && !p1_inside)
		{
			if (2 == intersections.size())
			{
				if (distance_sq(intersections[0], p0) > distance_sq(intersections[1], p0))
					std::swap(intersections[0], intersections[1]);
				return clipped_segment{ intersections[0], intersections[1], true };
			}
			return result;
		}

		if (!intersections.empty())
		{
			if (!p0_inside)
				return clipped_segment{ intersections[nearest_intersection(intersections, p1)], p1, true };
			if (!p1_inside)
				return clipped_segment{ p0, intersections[nearest_intersection(intersections, p0)], true };
		}

		return result;
	}

	void rebuild_segments()
	{
		m_segment_lengths.clear();
		m_total_length = 0.0;

		for (std::size_t i = 0; (i + 1) < m_points.size(); i += 2)
		{
			clipped_segment const clipped = clip_segment(m_points[i], m_points[i + 1]);
			if (clipped.valid)
			{
				double const dx = clipped.b.x - clipped.a.x;
				double const dy = clipped.b.y - clipped.a.y;
				double const length = std::sqrt((dx * dx) + (dy * dy));

				m_segment_lengths.push_back(length);
				m_total_length += length;
			}
			else
			{
				m_segment_lengths.push_back(0.0);
			}
		}

		if (m_total_length <= 0.0)
			m_total_length = 1.0e-6;
	}

	double random_signed()
	{
		m_random_state = (m_random_state * 1664525U) + 1013904223U;
		return ((double(m_random_state & 0x00ffffffU) / 16777215.0) * 2.0) - 1.0;
	}

	void generate(float *output, unsigned long frame_count)
	{
		std::fill_n(output, frame_count * 2U, 0.0f);

		if (m_next_frame_ready && (m_points.size() < 2U))
			activate_pending_frame();

		if ((m_points.size() < 2U) || m_segment_lengths.empty())
			return;

		double const scale = (double(frame_count) / m_total_length) / 4.0;

		for (unsigned long i = 0; i < frame_count; ++i)
		{
			m_frame_started = true;

			scope_point const &p0 = m_points[m_segment_index];
			scope_point const &p1 = m_points[m_segment_index + 1U];
			clipped_segment const clipped = clip_segment(p0, p1);

			if (!clipped.valid)
			{
				advance_segment();
				output[(i * 2U) + 0U] = float(random_signed());
				output[(i * 2U) + 1U] = float(random_signed());
				continue;
			}

			double const dx = clipped.b.x - clipped.a.x;
			double const dy = clipped.b.y - clipped.a.y;
			double const di = clipped.b.intensity - clipped.a.intensity;
			double length = m_segment_lengths[m_segment_index / 2U];
			int samples_per_segment;
			double t;
			double brightness;
			double speed_factor;

			if (length <= 1.0e-9)
				length = 0.05;

			samples_per_segment = std::max(int(std::floor(length * scale)), m_min_samples_per_segment);
			t = m_segment_pos / double(samples_per_segment);
			brightness = clipped.a.intensity + (di * t);
			speed_factor = std::max(std::exp(-brightness) * m_speed_scale, 0.0001);

			output[(i * 2U) + 0U] = float(clipped.a.x + (dx * t));
			output[(i * 2U) + 1U] = float(clipped.a.y + (dy * t));

			m_segment_pos += speed_factor;
			if (m_segment_pos > double(samples_per_segment + m_blank_samples))
				advance_segment();
		}
	}

	void queue_buffer(ALuint buffer_id)
	{
		generate(m_pcm.data(), static_cast<unsigned long>(m_frame_samples));
		m_openal.alBufferData(
				buffer_id,
				AL_FORMAT_STEREO_FLOAT32,
				m_pcm.data(),
				static_cast<ALsizei>(m_pcm.size() * sizeof(float)),
				static_cast<ALsizei>(m_sample_rate));
		m_openal.alSourceQueueBuffers(m_source_id, 1, &buffer_id);
	}

	void service_openal()
	{
		if (!m_started || !m_source_id)
			return;

		ALint state = 0;
		ALint processed = 0;

		m_openal.alGetSourcei(m_source_id, AL_SOURCE_STATE, &state);
		if (state == AL_STOPPED)
			m_openal.alSourcePlay(m_source_id);

		m_openal.alGetSourcei(m_source_id, AL_BUFFERS_PROCESSED, &processed);
		while (processed-- > 0)
		{
			ALuint buffer_id = 0;
			m_openal.alSourceUnqueueBuffers(m_source_id, 1, &buffer_id);
			if (buffer_id)
				queue_buffer(buffer_id);
		}
	}

	void advance_segment()
	{
		m_segment_pos = 0.0;
		m_segment_index += 2U;
		if (m_segment_index >= m_points.size())
		{
			m_segment_index = 0U;
			if (m_frame_started && m_next_frame_ready)
				activate_pending_frame();
		}
	}

	void activate_pending_frame()
	{
		if (!m_next_frame_ready)
			return;

		m_points = m_next_points;
		m_next_points.clear();
		m_next_frame_ready = false;
		m_segment_index = 0U;
		m_segment_pos = 0.0;
		m_frame_started = false;
		rebuild_segments();
	}

	running_machine &m_machine;
	screen_device &m_screen;
	openal_api m_openal;
	std::vector<scope_point> m_pending_points;
	std::vector<scope_point> m_next_points;
	std::vector<scope_point> m_points;
	std::vector<double> m_segment_lengths;
	std::vector<ALuint> m_buffer_ids;
	std::vector<float> m_pcm;
	std::size_t m_segment_index = 0U;
	uint32_t m_random_state = 0x12345678U;
	int m_sample_rate = 44100;
	int m_frame_samples = 735;
	int m_blank_samples = 0;
	int m_min_samples_per_segment = 2;
	int m_buffer_count = 8;
	double m_segment_pos = 0.0;
	double m_speed_scale = 1.0;
	double m_total_length = 1.0e-6;
	bool m_next_frame_ready = false;
	bool m_frame_started = false;
	bool m_started = false;
	ALCdevice *m_device = nullptr;
	ALCcontext *m_context = nullptr;
	ALuint m_source_id = 0;
};


starwars_state::starwars_state(const machine_config &mconfig, device_type type, const char *tag) :
	driver_device(mconfig, type, tag),
	m_vector(*this, "vector"),
	m_screen(*this, "screen"),
	m_soundlatch(*this, "soundlatch"),
	m_mainlatch(*this, "mainlatch"),
	m_riot(*this, "riot"),
	m_mathram(*this, "mathram"),
	m_maincpu(*this, "maincpu"),
	m_audiocpu(*this, "audiocpu"),
	m_pokey(*this, "pokey%u", 1U),
	m_tms(*this, "tms"),
	m_novram(*this, "x2212"),
	m_slapstic(*this, "slapstic"),
	m_slapstic_bank(*this, "slapstic_bank")
{
}


starwars_state::~starwars_state() = default;



/*************************************
 *
 *  Machine init
 *
 *************************************/

void starwars_state::machine_reset()
{
	/* reset the matrix processor */
	starwars_mproc_reset();
}


void starwars_state::machine_start()
{
	driver_device::machine_start();
	machine().add_notifier(MACHINE_NOTIFY_EXIT, machine_notify_delegate(&starwars_state::scope_output_stop, this));

	#if defined(_WIN32)
	if (machine().options().value(WINOPTION_XY_SCOPE_DEVICE)[0])
	{
		m_scope_output = std::make_unique<starwars_scope_openal_output>(machine(), *m_screen);
		if (m_scope_output->start())
		{
			m_scope_frame_begin = m_vector->add_frame_begin_notifier([this] () { m_scope_output->frame_begin(); });
			m_scope_line = m_vector->add_line_notifier(
					[this] (int x0, int y0, int x1, int y1, uint32_t, int intensity, int, int)
					{
						m_scope_output->add_line(x0, y0, x1, y1, intensity);
					});
			m_scope_frame_end = m_vector->add_frame_end_notifier([this] () { m_scope_output->frame_end(); });
		}
		else
		{
			m_scope_output.reset();
		}
	}
	#endif
}


void starwars_state::scope_output_stop()
{
	m_scope_frame_begin.reset();
	m_scope_line.reset();
	m_scope_frame_end.reset();

	if (m_scope_output)
	{
		m_scope_output->stop();
		m_scope_output.reset();
	}
}



/*************************************
 *
 *  Interrupt generation
 *
 *************************************/

void starwars_state::irq_ack_w(uint8_t data)
{
	m_maincpu->set_input_line(M6809_IRQ_LINE, CLEAR_LINE);
}



/*************************************
 *
 *  Main CPU memory handlers
 *
 *************************************/

uint8_t starwars_state::starwars_main_ready_flag_r()
{
	/* only upper two flag bits mapped */
	return (m_soundlatch->pending_r() << 7) | (m_mainlatch->pending_r() << 6);
}

void starwars_state::starwars_soundrst_w(uint8_t data)
{
	m_soundlatch->acknowledge_w();
	m_mainlatch->acknowledge_w();

	/* reset sound CPU here  */
	m_audiocpu->pulse_input_line(INPUT_LINE_RESET, attotime::zero);
}

void starwars_state::main_map(address_map &map)
{
	map(0x0000, 0x2fff).ram();
	map(0x3000, 0x3fff).rom().region("vectorrom", 0);
	map(0x4300, 0x431f).portr("IN0");
	map(0x4320, 0x433f).portr("IN1");
	map(0x4340, 0x435f).portr("DSW0");
	map(0x4360, 0x437f).portr("DSW1");
	map(0x4380, 0x439f).r("adc", FUNC(adc0809_device::data_r));
	map(0x4400, 0x4400).r(m_mainlatch, FUNC(generic_latch_8_device::read));
	map(0x4400, 0x4400).w(m_soundlatch, FUNC(generic_latch_8_device::write));
	map(0x4401, 0x4401).r(FUNC(starwars_state::starwars_main_ready_flag_r));
	map(0x4500, 0x45ff).rw("x2212", FUNC(x2212_device::read), FUNC(x2212_device::write));
	map(0x4600, 0x461f).w("avg", FUNC(avg_starwars_device::go_w));
	map(0x4620, 0x463f).w("avg", FUNC(avg_starwars_device::reset_w));
	map(0x4640, 0x465f).w("watchdog", FUNC(watchdog_timer_device::reset_w));
	map(0x4660, 0x467f).w(FUNC(starwars_state::irq_ack_w));
	map(0x4680, 0x4687).nopr().mirror(0x0018).w("outlatch", FUNC(ls259_device::write_d7));
	map(0x46a0, 0x46bf).w(FUNC(starwars_state::starwars_nstore_w));
	map(0x46c0, 0x46c3).w("adc", FUNC(adc0809_device::address_offset_start_w));
	map(0x46e0, 0x46e0).w(FUNC(starwars_state::starwars_soundrst_w));
	map(0x4700, 0x4707).w(FUNC(starwars_state::starwars_math_w));
	map(0x4700, 0x4700).r(FUNC(starwars_state::starwars_div_reh_r));
	map(0x4701, 0x4701).r(FUNC(starwars_state::starwars_div_rel_r));
	map(0x4703, 0x4703).r(FUNC(starwars_state::starwars_prng_r));           /* pseudo random number generator */
	map(0x4800, 0x4fff).ram();                             /* CPU and Math RAM */
	map(0x5000, 0x5fff).ram().share("mathram"); /* CPU and Math RAM */
	map(0x6000, 0x7fff).bankr("bank1");                        /* banked ROM */
	map(0x8000, 0xffff).rom();                             /* rest of main_rom */
}

void starwars_state::esb_main_map(address_map &map)
{
	main_map(map);
	map(0x8000, 0x9fff).bankr(m_slapstic_bank);
	map(0xa000, 0xffff).bankr("bank2");
}


/*************************************
 *
 *  Sound CPU memory handlers
 *
 *************************************/

void starwars_state::quad_pokeyn_w(offs_t offset, uint8_t data)
{
	int pokey_num = (offset >> 3) & ~0x04;
	int control = (offset & 0x20) >> 2;
	int pokey_reg = (offset % 8) | control;

	m_pokey[pokey_num]->write(pokey_reg, data);
}

void starwars_state::sound_map(address_map &map)
{
	map(0x0000, 0x07ff).w(m_mainlatch, FUNC(generic_latch_8_device::write));
	map(0x0800, 0x0fff).r(m_soundlatch, FUNC(generic_latch_8_device::read)); /* SIN Read */
	map(0x1000, 0x107f).m(m_riot, FUNC(mos6532_device::ram_map));
	map(0x1080, 0x109f).m(m_riot, FUNC(mos6532_device::io_map));
	map(0x1800, 0x183f).w(FUNC(starwars_state::quad_pokeyn_w));
	map(0x2000, 0x27ff).ram();                         /* program RAM */
	map(0x4000, 0x7fff).rom();                         /* sound roms */
	map(0xb000, 0xffff).rom();                         /* more sound roms */
}



/*************************************
 *
 *  Port definitions
 *
 *  Dips Manual Verified and Defaults
 *  set for starwars and esb - 06/2009
 *
 *************************************/

static INPUT_PORTS_START( starwars )
	PORT_START("IN0")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_COIN2 )
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_COIN1 )
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_SERVICE1 )
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_TILT )
	PORT_SERVICE( 0x10, IP_ACTIVE_LOW )
	PORT_BIT( 0x20, IP_ACTIVE_HIGH, IPT_UNUSED )
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_BUTTON4 )
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_BUTTON1 )

	PORT_START("IN1")
	PORT_BIT( 0x01, IP_ACTIVE_HIGH, IPT_UNUSED )
	PORT_BIT( 0x02, IP_ACTIVE_HIGH, IPT_UNUSED )
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_SERVICE2 ) PORT_NAME("Diagnostic Step") // mentioned in schematics, but N/C?
	PORT_BIT( 0x08, IP_ACTIVE_HIGH, IPT_UNUSED )
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_BUTTON3 )
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_BUTTON2 )
	/* Bit 6 is VG_HALT */
	PORT_BIT( 0x40, IP_ACTIVE_HIGH, IPT_CUSTOM ) PORT_READ_LINE_DEVICE_MEMBER("avg", FUNC(avg_starwars_device::done_r))
	/* Bit 7 is MATH_RUN - see machine/starwars.c */
	PORT_BIT( 0x80, IP_ACTIVE_HIGH, IPT_CUSTOM ) PORT_READ_LINE_MEMBER(FUNC(starwars_state::matrix_flag_r))

	PORT_START("DSW0")
	PORT_DIPNAME( 0x03, 0x00, "Starting Shields" )  PORT_DIPLOCATION("10D:1,2")
	PORT_DIPSETTING(    0x00, "6" )
	PORT_DIPSETTING(    0x01, "7" )
	PORT_DIPSETTING(    0x02, "8" )
	PORT_DIPSETTING(    0x03, "9" )
	PORT_DIPNAME( 0x0c, 0x08, DEF_STR( Difficulty ) ) PORT_DIPLOCATION("10D:3,4")
	PORT_DIPSETTING(    0x00, DEF_STR( Easy ) )
	PORT_DIPSETTING(    0x04, "Moderate" )
	PORT_DIPSETTING(    0x08, DEF_STR( Hard ) )
	PORT_DIPSETTING(    0x0c, DEF_STR( Hardest ) )
	PORT_DIPNAME( 0x30, 0x10, "Bonus Shields" )  PORT_DIPLOCATION("10D:5,6")
	PORT_DIPSETTING(    0x00, "0" )
	PORT_DIPSETTING(    0x10, "1" )
	PORT_DIPSETTING(    0x20, "2" )
	PORT_DIPSETTING(    0x30, "3" )
	PORT_DIPNAME( 0x40, 0x00, DEF_STR( Demo_Sounds ) ) PORT_DIPLOCATION("10D:7")
	PORT_DIPSETTING(    0x40, DEF_STR( Off ) )
	PORT_DIPSETTING(    0x00, DEF_STR( On ) )
	PORT_DIPNAME( 0x80, 0x80, "Freeze" )  PORT_DIPLOCATION("10D:8")
	PORT_DIPSETTING(    0x80, DEF_STR( Off ) )
	PORT_DIPSETTING(    0x00, DEF_STR( On ) )

	PORT_START("DSW1")
	PORT_DIPNAME( 0x03, 0x02, DEF_STR( Coinage ) )  PORT_DIPLOCATION("10EF:1,2")
	PORT_DIPSETTING(    0x03, DEF_STR( 2C_1C ) )
	PORT_DIPSETTING(    0x02, DEF_STR( 1C_1C ) )
	PORT_DIPSETTING(    0x01, DEF_STR( 1C_2C ) )
	PORT_DIPSETTING(    0x00, DEF_STR( Free_Play ) )
		/* Manual shows Coin_B (Right) as Bit 4,5 - actually Bit 3,4 */
	PORT_DIPNAME( 0x0c, 0x00, DEF_STR( Coin_B ) )  PORT_DIPLOCATION("10EF:3,4")
	PORT_DIPSETTING(    0x00, "*1" )
	PORT_DIPSETTING(    0x04, "*4" )
	PORT_DIPSETTING(    0x08, "*5" )
	PORT_DIPSETTING(    0x0c, "*6" )
		/* Manual shows Coin_A (Left) as Bit 3 - actually Bit 5 */
	PORT_DIPNAME( 0x10, 0x00, DEF_STR( Coin_A ) )  PORT_DIPLOCATION("10EF:5")
	PORT_DIPSETTING(    0x00, "*1" )
	PORT_DIPSETTING(    0x10, "*2" )
	PORT_DIPNAME( 0xe0, 0x00, "Bonus Coin Adder" )  PORT_DIPLOCATION("10EF:6,7,8")
	PORT_DIPSETTING(    0x20, "2 gives 1" )
	PORT_DIPSETTING(    0x60, "4 gives 2" )
	PORT_DIPSETTING(    0xa0, "3 gives 1" )
	PORT_DIPSETTING(    0x40, "4 gives 1" )
	PORT_DIPSETTING(    0x80, "5 gives 1" )
	PORT_DIPSETTING(    0x00, DEF_STR( None ) )
	/* 0xc0 and 0xe0 None */

	PORT_START("STICKY")
	PORT_BIT( 0xff, 0x80, IPT_AD_STICK_Y ) PORT_SENSITIVITY(70) PORT_KEYDELTA(30)

	PORT_START("STICKX")
	PORT_BIT( 0xff, 0x80, IPT_AD_STICK_X ) PORT_SENSITIVITY(50) PORT_KEYDELTA(30)
INPUT_PORTS_END


static INPUT_PORTS_START( esb )
	PORT_INCLUDE( starwars )

	PORT_MODIFY("DSW0")
	PORT_DIPNAME( 0x03, 0x03, "Starting Shields" )  PORT_DIPLOCATION("10D:1,2")
	PORT_DIPSETTING(    0x01, "2" )
	PORT_DIPSETTING(    0x00, "3" )
	PORT_DIPSETTING(    0x03, "4" )
	PORT_DIPSETTING(    0x02, "5" )
	PORT_DIPNAME( 0x0c, 0x00, DEF_STR( Difficulty ) ) PORT_DIPLOCATION("10D:3,4")
	PORT_DIPSETTING(    0x08, DEF_STR( Easy ) )
	PORT_DIPSETTING(    0x0c, "Moderate" )
	PORT_DIPSETTING(    0x00, DEF_STR( Hard ) )
	PORT_DIPSETTING(    0x04, DEF_STR( Hardest ) )
	PORT_DIPNAME( 0x30, 0x30, "Jedi-Letter Mode" )  PORT_DIPLOCATION("10D:5,6")
	PORT_DIPSETTING(    0x00, "Level Only" )
	PORT_DIPSETTING(    0x10, "Level" )
	PORT_DIPSETTING(    0x20, "Increment Only" )
	PORT_DIPSETTING(    0x30, "Increment" )
	PORT_DIPNAME( 0x40, 0x40, DEF_STR( Demo_Sounds ) ) PORT_DIPLOCATION("10D:7")
	PORT_DIPSETTING(    0x00, DEF_STR( Off ) ) // "No Music In Attract Mode" switch 'on'
	PORT_DIPSETTING(    0x40, DEF_STR( On ) ) // "Music In Attract Mode" switch 'off'
INPUT_PORTS_END



/*************************************
 *
 *  Machine driver
 *
 *************************************/

void starwars_state::starwars(machine_config &config)
{
	/* basic machine hardware */
	MC6809E(config, m_maincpu, MASTER_CLOCK / 8);
	m_maincpu->set_addrmap(AS_PROGRAM, &starwars_state::main_map);
	m_maincpu->set_periodic_int(FUNC(starwars_state::irq0_line_assert), attotime::from_hz(CLOCK_3KHZ / 12));

	WATCHDOG_TIMER(config, "watchdog").set_time(attotime::from_hz(CLOCK_3KHZ / 128));

	MC6809E(config, m_audiocpu, MASTER_CLOCK / 8);
	m_audiocpu->set_addrmap(AS_PROGRAM, &starwars_state::sound_map);

	adc0809_device &adc(ADC0809(config, "adc", MASTER_CLOCK / 16)); // designated as "137243-001" on parts list and "157249-120" on schematics
	adc.in_callback<0>().set_ioport("STICKY"); // pitch
	adc.in_callback<1>().set_ioport("STICKX"); // yaw
	adc.in_callback<2>().set_constant(0); // thrust (unused)

	MOS6532(config, m_riot, MASTER_CLOCK / 8);
	m_riot->pa_wr_callback<0>().set(m_tms, FUNC(tms5220_device::wsq_w));
	m_riot->pa_wr_callback<1>().set(m_tms, FUNC(tms5220_device::rsq_w));
	m_riot->pa_rd_callback<2>().set(m_tms, FUNC(tms5220_device::readyq_r));
	m_riot->pa_wr_callback<3>().set_nop(); // hold main CPU in reset? + enable delay circuit?
	m_riot->pa_rd_callback<4>().set_constant(1); // not sound self test
	m_riot->pa_wr_callback<5>().set_nop(); // TMS5220 VDD
	m_riot->pb_rd_callback().set(m_tms, FUNC(tms5220_device::status_r));
	m_riot->pb_wr_callback().set(m_tms, FUNC(tms5220_device::data_w));
	m_riot->irq_wr_callback().set_inputline(m_audiocpu, M6809_IRQ_LINE);

	X2212(config, "x2212").set_auto_save(true); /* nvram */

	ls259_device &outlatch(LS259(config, "outlatch")); // 9L/M
	outlatch.q_out_cb<0>().set(FUNC(starwars_state::coin1_counter_w)); // Coin counter 1
	outlatch.q_out_cb<1>().set(FUNC(starwars_state::coin2_counter_w)); // Coin counter 2
	outlatch.q_out_cb<2>().set_output("led2").invert(); // LED 3
	outlatch.q_out_cb<3>().set_output("led1").invert(); // LED 2
	outlatch.q_out_cb<4>().set_membank("bank1"); // bank switch
	outlatch.q_out_cb<5>().set(FUNC(starwars_state::prng_reset_w)); // reset PRNG
	outlatch.q_out_cb<6>().set_output("led0").invert(); // LED 1
	outlatch.q_out_cb<7>().set(FUNC(starwars_state::recall_w)); // NVRAM array recall

	/* video hardware */
	VECTOR(config, "vector", 0);
	screen_device &screen(SCREEN(config, "screen", SCREEN_TYPE_VECTOR));
	screen.set_refresh_hz(CLOCK_3KHZ / 12 / 6);
	screen.set_size(400, 300);
	screen.set_visarea(0, 250, 0, 280);
	screen.set_screen_update("vector", FUNC(vector_device::screen_update));

	avg_device &avg(AVG_STARWARS(config, "avg", 0));
	avg.set_vector("vector");
	avg.set_memory(m_maincpu, AS_PROGRAM, 0x0000);

	/* sound hardware */
	SPEAKER(config, "mono").front_center();

	POKEY(config, m_pokey[0], MASTER_CLOCK / 8).add_route(ALL_OUTPUTS, "mono", 0.20);
	POKEY(config, m_pokey[1], MASTER_CLOCK / 8).add_route(ALL_OUTPUTS, "mono", 0.20);
	POKEY(config, m_pokey[2], MASTER_CLOCK / 8).add_route(ALL_OUTPUTS, "mono", 0.20);
	POKEY(config, m_pokey[3], MASTER_CLOCK / 8).add_route(ALL_OUTPUTS, "mono", 0.20);

	TMS5220(config, m_tms, MASTER_CLOCK/2/9).add_route(ALL_OUTPUTS, "mono", 0.50);

	GENERIC_LATCH_8(config, m_soundlatch);
	m_soundlatch->data_pending_callback().set(m_riot, FUNC(mos6532_device::pa_bit_w<7>));
	m_soundlatch->data_pending_callback().append([this](int state) { if (state) machine().scheduler().perfect_quantum(attotime::from_usec(100)); });

	GENERIC_LATCH_8(config, m_mainlatch);
	m_mainlatch->data_pending_callback().set(m_riot, FUNC(mos6532_device::pa_bit_w<6>));
	m_mainlatch->data_pending_callback().append([this](int state) { if (state) machine().scheduler().perfect_quantum(attotime::from_usec(100)); });
}


void starwars_state::esb(machine_config &config)
{
	starwars(config);

	m_maincpu->set_addrmap(AS_PROGRAM, &starwars_state::esb_main_map);

	SLAPSTIC(config, m_slapstic, 101);
	m_slapstic->set_range(m_maincpu, AS_PROGRAM, 0x8000, 0x9fff, 0);
	m_slapstic->set_bank(m_slapstic_bank);

	subdevice<ls259_device>("outlatch")->q_out_cb<4>().append_membank("bank2");
}



/*************************************
 *
 *  ROM definitions
 *
 *************************************/

ROM_START( starwars )
	ROM_REGION( 0x12000, "maincpu", 0 )     /* 2 64k ROM spaces */
	ROM_LOAD( "136021.214.1f", 0x6000, 0x2000, CRC(04f1876e) SHA1(c1d3637cb31ece0890c25f6122d6bcd27e6ffe0c) ) /* ROM 0 bank pages 0 and 1 */
	ROM_CONTINUE(              0x10000, 0x2000 )
	ROM_LOAD( "136021.102.1hj",0x8000, 0x2000, CRC(f725e344) SHA1(f8943b67f2ea032ab9538084756ba86f892be5ca) ) /*  8k ROM 1 bank */
	ROM_LOAD( "136021.203.1jk",0xa000, 0x2000, CRC(f6da0a00) SHA1(dd53b643be856787bbc4da63e5eb132f98f623c3) ) /*  8k ROM 2 bank */
	ROM_LOAD( "136021.104.1kl",0xc000, 0x2000, CRC(7e406703) SHA1(981b505d6e06d7149f8bcb3e81e4d0c790f2fc86) ) /*  8k ROM 3 bank */
	ROM_LOAD( "136021.206.1m", 0xe000, 0x2000, CRC(c7e51237) SHA1(4960f4446271316e3f730eeb2531dbc702947395) ) /*  8k ROM 4 bank */

	ROM_REGION( 0x1000, "vectorrom", 0 )
	ROM_LOAD( "136021-105.1l", 0x0000, 0x1000, CRC(538e7d2f) SHA1(032c933fd94a6b0b294beee29159a24494ae969b) ) /* 3000-3fff is 4k vector rom */

	/* Sound ROMS */
	ROM_REGION( 0x10000, "audiocpu", 0 )
	ROM_LOAD( "136021-107.1jk",0x4000, 0x2000, CRC(dbf3aea2) SHA1(c38661b2b846fe93487eef09ca3cda19c44f08a0) ) /* Sound ROM 0 */
	ROM_RELOAD(                0xc000, 0x2000 )
	ROM_LOAD( "136021-208.1h", 0x6000, 0x2000, CRC(e38070a8) SHA1(c858ae1702efdd48615453ab46e488848891d139) ) /* Sound ROM 0 */
	ROM_RELOAD(                0xe000, 0x2000 )

	ROM_REGION( 0x100, "avg:prom", 0)
	ROM_LOAD( "136021-109.4b", 0x0000, 0x0100, CRC(82fc3eb2) SHA1(184231c7baef598294860a7d2b8a23798c5c7da6) ) /* AVG PROM */

	/* Mathbox PROMs */
	ROM_REGION( 0x1000, "user2", 0 )
	ROM_LOAD( "136021-110.7h", 0x0000, 0x0400, CRC(810e040e) SHA1(d247cbb0afb4538d5161f8ce9eab337cdb3f2da4) ) /* PROM 0 */
	ROM_LOAD( "136021-111.7j", 0x0400, 0x0400, CRC(ae69881c) SHA1(f3420c6e15602956fd94982a5d8d4ddd015ed977) ) /* PROM 1 */
	ROM_LOAD( "136021-112.7k", 0x0800, 0x0400, CRC(ecf22628) SHA1(4dcf5153221feca329b8e8d199bd4fc00b151d9c) ) /* PROM 2 */
	ROM_LOAD( "136021-113.7l", 0x0c00, 0x0400, CRC(83febfde) SHA1(e13541b09d1724204fdb171528e9a1c83c799c1c) ) /* PROM 3 */
ROM_END

ROM_START( starwars1 )
	ROM_REGION( 0x12000, "maincpu", 0 )     /* 2 64k ROM spaces */
	ROM_LOAD( "136021.114.1f", 0x6000, 0x2000, CRC(e75ff867) SHA1(3a40de920c31ffa3c3e67f3edf653b79fcc5ddd7) ) /* ROM 0 bank pages 0 and 1 */
	ROM_CONTINUE(              0x10000, 0x2000 )
	ROM_LOAD( "136021.102.1hj",0x8000, 0x2000, CRC(f725e344) SHA1(f8943b67f2ea032ab9538084756ba86f892be5ca) ) /*  8k ROM 1 bank */
	ROM_LOAD( "136021.203.1jk",0xa000, 0x2000, CRC(f6da0a00) SHA1(dd53b643be856787bbc4da63e5eb132f98f623c3) ) /*  8k ROM 2 bank */
	ROM_LOAD( "136021.104.1kl",0xc000, 0x2000, CRC(7e406703) SHA1(981b505d6e06d7149f8bcb3e81e4d0c790f2fc86) ) /*  8k ROM 3 bank */
	ROM_LOAD( "136021.206.1m", 0xe000, 0x2000, CRC(c7e51237) SHA1(4960f4446271316e3f730eeb2531dbc702947395) ) /*  8k ROM 4 bank */

	ROM_REGION( 0x1000, "vectorrom", 0 )
	ROM_LOAD( "136021-105.1l", 0x0000, 0x1000, CRC(538e7d2f) SHA1(032c933fd94a6b0b294beee29159a24494ae969b) ) /* 3000-3fff is 4k vector rom */

	/* Sound ROMS */
	ROM_REGION( 0x10000, "audiocpu", 0 )
	ROM_LOAD( "136021-107.1jk",0x4000, 0x2000, CRC(dbf3aea2) SHA1(c38661b2b846fe93487eef09ca3cda19c44f08a0) ) /* Sound ROM 0 */
	ROM_RELOAD(                0xc000, 0x2000 )
	ROM_LOAD( "136021-208.1h", 0x6000, 0x2000, CRC(e38070a8) SHA1(c858ae1702efdd48615453ab46e488848891d139) ) /* Sound ROM 0 */
	ROM_RELOAD(                0xe000, 0x2000 )

	ROM_REGION( 0x100, "avg:prom", 0)
	ROM_LOAD( "136021-109.4b", 0x0000, 0x0100, CRC(82fc3eb2) SHA1(184231c7baef598294860a7d2b8a23798c5c7da6) ) /* AVG PROM */

	/* Mathbox PROMs */
	ROM_REGION( 0x1000, "user2", 0)
	ROM_LOAD( "136021-110.7h", 0x0000, 0x0400, CRC(810e040e) SHA1(d247cbb0afb4538d5161f8ce9eab337cdb3f2da4) ) /* PROM 0 */
	ROM_LOAD( "136021-111.7j", 0x0400, 0x0400, CRC(ae69881c) SHA1(f3420c6e15602956fd94982a5d8d4ddd015ed977) ) /* PROM 1 */
	ROM_LOAD( "136021-112.7k", 0x0800, 0x0400, CRC(ecf22628) SHA1(4dcf5153221feca329b8e8d199bd4fc00b151d9c) ) /* PROM 2 */
	ROM_LOAD( "136021-113.7l", 0x0c00, 0x0400, CRC(83febfde) SHA1(e13541b09d1724204fdb171528e9a1c83c799c1c) ) /* PROM 3 */
ROM_END

ROM_START( starwarso )
	ROM_REGION( 0x12000, "maincpu", 0 )     /* 2 64k ROM spaces */
	ROM_LOAD( "136021-114.1f", 0x6000, 0x2000, CRC(e75ff867) SHA1(3a40de920c31ffa3c3e67f3edf653b79fcc5ddd7) ) /* ROM 0 bank pages 0 and 1 */
	ROM_CONTINUE(              0x10000, 0x2000 )
	ROM_LOAD( "136021-102.1hj",0x8000, 0x2000, CRC(f725e344) SHA1(f8943b67f2ea032ab9538084756ba86f892be5ca) ) /*  8k ROM 1 bank */
	ROM_LOAD( "136021-103.1jk",0xa000, 0x2000, CRC(3fde9ccb) SHA1(8d88fc7a28ac8f189f8aba08598732ac8c5491aa) ) /*  8k ROM 2 bank */
	ROM_LOAD( "136021-104.1kl",0xc000, 0x2000, CRC(7e406703) SHA1(981b505d6e06d7149f8bcb3e81e4d0c790f2fc86) ) /*  8k ROM 3 bank */
	ROM_LOAD( "136021-206.1m", 0xe000, 0x2000, CRC(c7e51237) SHA1(4960f4446271316e3f730eeb2531dbc702947395) ) /*  8k ROM 4 bank */

	ROM_REGION( 0x1000, "vectorrom", 0 )
	ROM_LOAD( "136021-105.1l", 0x0000, 0x1000, CRC(538e7d2f) SHA1(032c933fd94a6b0b294beee29159a24494ae969b) ) /* 3000-3fff is 4k vector rom */

	/* Sound ROMS */
	ROM_REGION( 0x10000, "audiocpu", 0 )
	ROM_LOAD( "136021-107.1jk",0x4000, 0x2000, CRC(dbf3aea2) SHA1(c38661b2b846fe93487eef09ca3cda19c44f08a0) ) /* Sound ROM 0 */
	ROM_RELOAD(                0xc000, 0x2000 )
	ROM_LOAD( "136021-208.1h", 0x6000, 0x2000, CRC(e38070a8) SHA1(c858ae1702efdd48615453ab46e488848891d139) ) /* Sound ROM 0 */
	ROM_RELOAD(                0xe000, 0x2000 )

	ROM_REGION( 0x100, "avg:prom", 0)
	ROM_LOAD( "136021-109.4b", 0x0000, 0x0100, CRC(82fc3eb2) SHA1(184231c7baef598294860a7d2b8a23798c5c7da6) ) /* AVG PROM */

	/* Mathbox PROMs */
	ROM_REGION( 0x1000, "user2", 0 )
	ROM_LOAD( "136021-110.7h", 0x0000, 0x0400, CRC(810e040e) SHA1(d247cbb0afb4538d5161f8ce9eab337cdb3f2da4) ) /* PROM 0 */
	ROM_LOAD( "136021-111.7j", 0x0400, 0x0400, CRC(ae69881c) SHA1(f3420c6e15602956fd94982a5d8d4ddd015ed977) ) /* PROM 1 */
	ROM_LOAD( "136021-112.7k", 0x0800, 0x0400, CRC(ecf22628) SHA1(4dcf5153221feca329b8e8d199bd4fc00b151d9c) ) /* PROM 2 */
	ROM_LOAD( "136021-113.7l", 0x0c00, 0x0400, CRC(83febfde) SHA1(e13541b09d1724204fdb171528e9a1c83c799c1c) ) /* PROM 3 */
ROM_END



ROM_START( tomcatsw )
	ROM_REGION( 0x12000, "maincpu", 0 )
	ROM_LOAD( "tc6.1f",        0x6000, 0x2000, CRC(56e284ff) SHA1(a5fda9db0f6b8f7d28a4a607976fe978e62158cf) )
	ROM_LOAD( "tc8.1hj",       0x8000, 0x2000, CRC(7b7575e3) SHA1(bdb838603ffb12195966d0ce454900253bc0f43f) )
	ROM_LOAD( "tca.1jk",       0xa000, 0x2000, CRC(a1020331) SHA1(128745a2ec771ac818a8fbba59a08f0cf5f28e8f) )
	ROM_LOAD( "tce.1m",        0xe000, 0x2000, CRC(4a3de8a3) SHA1(e48fc17201326358317f6b428e583ecaa3ecb881) )

	ROM_REGION( 0x1000, "vectorrom", 0 )
	ROM_LOAD( "tcavg3.1l",     0x0000, 0x1000, CRC(27188aa9) SHA1(5d9a978a7ac1913b57586e81045a1b955db27b48) )

	/* Sound ROMS */
	ROM_REGION( 0x10000, "audiocpu", ROMREGION_ERASE00 )

	ROM_REGION( 0x100, "avg:prom", 0)
	ROM_LOAD( "136021-109.4b", 0x0000, 0x0100, CRC(82fc3eb2) SHA1(184231c7baef598294860a7d2b8a23798c5c7da6) ) /* AVG PROM */

	/* Mathbox PROMs */
	ROM_REGION( 0x1000, "user2", 0 )
	ROM_LOAD( "136021-110.7h", 0x0000, 0x0400, CRC(810e040e) SHA1(d247cbb0afb4538d5161f8ce9eab337cdb3f2da4) ) /* PROM 0 */
	ROM_LOAD( "136021-111.7j", 0x0400, 0x0400, CRC(ae69881c) SHA1(f3420c6e15602956fd94982a5d8d4ddd015ed977) ) /* PROM 1 */
	ROM_LOAD( "136021-112.7k", 0x0800, 0x0400, CRC(ecf22628) SHA1(4dcf5153221feca329b8e8d199bd4fc00b151d9c) ) /* PROM 2 */
	ROM_LOAD( "136021-113.7l", 0x0c00, 0x0400, CRC(83febfde) SHA1(e13541b09d1724204fdb171528e9a1c83c799c1c) ) /* PROM 3 */
ROM_END


ROM_START( esb )
	ROM_REGION( 0x22000, "maincpu", 0 )     /* 64k for code and a buttload for the banked ROMs */
	ROM_LOAD( "136031-101.1f", 0x6000, 0x2000, CRC(ef1e3ae5) SHA1(d228ff076faa7f9605badeee3b827adb62593e0a) )
	ROM_CONTINUE(              0x10000, 0x2000 )
	/* $8000 - $9fff : slapstic page */
	ROM_LOAD( "136031-102.1jk",0xa000, 0x2000, CRC(62ce5c12) SHA1(976256acf4499dc396542a117910009a8808f448) )
	ROM_CONTINUE(              0x1c000, 0x2000 )
	ROM_LOAD( "136031-203.1kl",0xc000, 0x2000, CRC(27b0889b) SHA1(a13074e83f0f57d65096d7f49ae78f33ab00c479) )
	ROM_CONTINUE(              0x1e000, 0x2000 )
	ROM_LOAD( "136031-104.1m", 0xe000, 0x2000, CRC(fd5c725e) SHA1(541cfd004b1736b6cec13836dfa813f00eedeed0) )
	ROM_CONTINUE(              0x20000, 0x2000 )

	ROM_LOAD( "136031-105.3u", 0x14000, 0x4000, CRC(ea9e4dce) SHA1(9363fd5b1fce62c2306b448a7766eaf7ec97cdf5) ) /* slapstic 0, 1 */
	ROM_LOAD( "136031-106.2u", 0x18000, 0x4000, CRC(76d07f59) SHA1(44dd018b406f95e1512ce92923c2c87f1458844f) ) /* slapstic 2, 3 */

	ROM_REGION( 0x1000, "vectorrom", 0 )
	ROM_LOAD( "136031-111.1l", 0x0000, 0x1000, CRC(b1f9bd12) SHA1(76f15395c9fdcd80dd241307a377031a1f44e150) ) /* 3000-3fff is 4k vector rom */

	/* Sound ROMS */
	ROM_REGION( 0x10000, "audiocpu", 0 )
	ROM_LOAD( "136031-113.1jk",0x4000, 0x2000, CRC(24ae3815) SHA1(b1a93af76de79b902317eebbc50b400b1f8c1e3c) ) /* Sound ROM 0 */
	ROM_CONTINUE(              0xc000, 0x2000 )
	ROM_LOAD( "136031-112.1h", 0x6000, 0x2000, CRC(ca72d341) SHA1(52de5b82bb85d7c9caad2047e540d0748aa93ba5) ) /* Sound ROM 1 */
	ROM_CONTINUE(              0xe000, 0x2000 )

	ROM_REGION( 0x100, "avg:prom", 0)
	ROM_LOAD( "136021-109.4b", 0x0000, 0x0100, CRC(82fc3eb2) SHA1(184231c7baef598294860a7d2b8a23798c5c7da6) ) /* AVG PROM */

	/* Mathbox PROMs */
	ROM_REGION( 0x1000, "user2", 0 )
	ROM_LOAD( "136031-110.7h", 0x0000, 0x0400, CRC(b8d0f69d) SHA1(c196f1a592bd1ac482a81e23efa224d9dfaefc0a) ) /* PROM 0 */
	ROM_LOAD( "136031-109.7j", 0x0400, 0x0400, CRC(6a2a4d98) SHA1(cefca71f025f92a193c5a7d8b5ab8be10db2fd44) ) /* PROM 1 */
	ROM_LOAD( "136031-108.7k", 0x0800, 0x0400, CRC(6a76138f) SHA1(9ef7af898a3e29d03f35045901023615a6a55205) ) /* PROM 2 */
	ROM_LOAD( "136031-107.7l", 0x0c00, 0x0400, CRC(afbf6e01) SHA1(0a6438e6c106d98e5d67a019751e1584324f5e5c) ) /* PROM 3 */
ROM_END



/*************************************
 *
 *  Driver init
 *
 *************************************/

void starwars_state::init_starwars()
{
	/* prepare the mathbox */
	starwars_mproc_init();

	/* initialize banking */
	membank("bank1")->configure_entries(0, 2, memregion("maincpu")->base() + 0x6000, 0x10000 - 0x6000);
	membank("bank1")->set_entry(0);
}


void starwars_state::init_esb()
{
	uint8_t *rom = memregion("maincpu")->base();

	/* init the slapstic */
	m_slapstic_bank->configure_entries(0, 4, memregion("maincpu")->base() + 0x14000, 0x2000);

	/* prepare the matrix processor */
	starwars_mproc_init();

	/* initialize banking */
	membank("bank1")->configure_entries(0, 2, rom + 0x6000, 0x10000 - 0x6000);
	membank("bank1")->set_entry(0);
	membank("bank2")->configure_entries(0, 2, rom + 0xa000, 0x1c000 - 0xa000);
	membank("bank2")->set_entry(0);
}



/*************************************
 *
 *  Game drivers
 *
 *************************************/

GAME( 1983, starwars, 0,        starwars, starwars, starwars_state, init_starwars, ROT0, "Atari", "Star Wars (set 1)", 0 ) // newest
GAME( 1983, starwars1,starwars, starwars, starwars, starwars_state, init_starwars, ROT0, "Atari", "Star Wars (set 2)", 0 )
GAME( 1983, starwarso,starwars, starwars, starwars, starwars_state, init_starwars, ROT0, "Atari", "Star Wars (set 3)", 0 ) // oldest
// is there an even older starwars set with 136021-106.1m ?

GAME( 1983, tomcatsw, tomcat,   starwars, starwars, starwars_state, init_starwars, ROT0, "Atari", "TomCat (Star Wars hardware, prototype)", MACHINE_NO_SOUND )

GAME( 1985, esb,      0,        esb,      esb,      starwars_state, init_esb,      ROT0, "Atari Games", "The Empire Strikes Back", 0 )
