#include "mednafen/mednafen.h"
#include "mednafen/mempatcher.h"
#include "mednafen/git.h"
#include "mednafen/general.h"
#ifdef NEED_DEINTERLACER
#include	"mednafen/video/Deinterlacer.h"
#endif
#include "libretro.h"
#include "mednafen/snes/src/base.hpp"

static MDFNGI *game;

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;
retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static bool overscan;
static double last_sound_rate;
static MDFN_PixelFormat last_pixel_format;

static MDFN_Surface *surf;

static bool failed_init;

static void hookup_ports(bool force);

static bool initial_ports_hookup = false;

std::string retro_base_directory;
std::string retro_base_name;
std::string retro_save_directory;

static void set_basename(const char *path)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');

   if (base)
      retro_base_name = base + 1;
   else
      retro_base_name = path;

   retro_base_name = retro_base_name.substr(0, retro_base_name.find_last_of('.'));
}

#ifdef NEED_DEINTERLACER
static bool PrevInterlaced;
static Deinterlacer deint;
#endif

#define MEDNAFEN_CORE_NAME_MODULE "snes"
#define MEDNAFEN_CORE_NAME "Mednafen bSNES"
#define MEDNAFEN_CORE_VERSION "v0.9.26"
#define MEDNAFEN_CORE_EXTENSIONS "smc|fig|bs|st|sfc"
#define MEDNAFEN_CORE_TIMING_FPS 60.10
#define MEDNAFEN_CORE_GEOMETRY_BASE_W (game->nominal_width)
#define MEDNAFEN_CORE_GEOMETRY_BASE_H (game->nominal_height)
#define MEDNAFEN_CORE_GEOMETRY_MAX_W 512
#define MEDNAFEN_CORE_GEOMETRY_MAX_H 512
#define MEDNAFEN_CORE_GEOMETRY_ASPECT_RATIO (4.0 / 3.0)
#define FB_WIDTH 512
#define FB_HEIGHT 512


#define FB_MAX_HEIGHT FB_HEIGHT

static void check_system_specs(void)
{
   //FIXME/TODO - might be higher/lower
   unsigned level = 13;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init(void)
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else 
      log_cb = NULL;

   MDFNI_InitializeModule();

   const char *dir = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      retro_base_directory = dir;
      // Make sure that we don't have any lingering slashes, etc, as they break Windows.
      size_t last = retro_base_directory.find_last_not_of("/\\");
      if (last != std::string::npos)
         last++;

      retro_base_directory = retro_base_directory.substr(0, last);

      MDFNI_Initialize(retro_base_directory.c_str());
   }
   else
   {
      /* TODO: Add proper fallback */
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "System directory is not defined. Fallback on using same dir as ROM for system directory later ...\n");
      failed_init = true;
   }
   
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
	  // If save directory is defined use it, otherwise use system directory
      retro_save_directory = *dir ? dir : retro_base_directory;
      // Make sure that we don't have any lingering slashes, etc, as they break Windows.
      size_t last = retro_save_directory.find_last_not_of("/\\");
      if (last != std::string::npos)
         last++;

      retro_save_directory = retro_save_directory.substr(0, last);      
   }
   else
   {
      /* TODO: Add proper fallback */
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "Save directory is not defined. Fallback on using SYSTEM directory ...\n");
	  retro_save_directory = retro_base_directory;
   }      

#if defined(WANT_16BPP) && defined(FRONTEND_SUPPORTS_RGB565)
   enum retro_pixel_format rgb565 = RETRO_PIXEL_FORMAT_RGB565;
   if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565) && log_cb)
      log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
#endif

   if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
      perf_get_cpu_features_cb = perf_cb.get_cpu_features;
   else
      perf_get_cpu_features_cb = NULL;

   check_system_specs();
}

void retro_reset(void)
{
   game->DoSimpleCommand(MDFN_MSC_RESET);
}

bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t)
{
   return false;
}

static void set_volume (uint32_t *ptr, unsigned number)
{
   switch(number)
   {
      default:
         *ptr = number;
         break;
   }
}

static void check_variables(void)
{
   struct retro_variable var = {0};
}

#define MAX_PLAYERS 5
#define MAX_BUTTONS 12 
static uint8_t input_buf[MAX_PLAYERS][2];


static void hookup_ports(bool force)
{
   MDFNGI *currgame = game;

   if (initial_ports_hookup && !force)
      return;

   for (unsigned i = 0; i < MAX_PLAYERS; i++)
      currgame->SetInput(i, "gamepad", &input_buf[i][0]);

   initial_ports_hookup = true;
}

bool retro_load_game(const struct retro_game_info *info)
{
   if (failed_init)
      return false;

#ifdef WANT_32BPP
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Pixel format XRGB8888 not supported by platform, cannot use %s.\n", MEDNAFEN_CORE_NAME);
      return false;
   }
#endif

   overscan = false;
   environ_cb(RETRO_ENVIRONMENT_GET_OVERSCAN, &overscan);

   set_basename(info->path);

   game = MDFNI_LoadGame(MEDNAFEN_CORE_NAME_MODULE, info->path);
   if (!game)
      return false;

   MDFN_PixelFormat pix_fmt(MDFN_COLORSPACE_RGB, 16, 8, 0, 24);
   last_pixel_format = MDFN_PixelFormat();
   
   surf = new MDFN_Surface(NULL, FB_WIDTH, FB_HEIGHT, FB_WIDTH, pix_fmt);

#ifdef NEED_DEINTERLACER
	PrevInterlaced = false;
	deint.ClearState();
#endif

   hookup_ports(true);

   check_variables();

   return game;
}

void retro_unload_game()
{
   if (!game)
      return;

   MDFNI_CloseGame();
}



static void update_input(void)
{
   unsigned j;
   MDFNGI *currgame = (MDFNGI*)game;

   static unsigned map[] = {
      RETRO_DEVICE_ID_JOYPAD_B,
      RETRO_DEVICE_ID_JOYPAD_Y,
      RETRO_DEVICE_ID_JOYPAD_SELECT,
      RETRO_DEVICE_ID_JOYPAD_START,
      RETRO_DEVICE_ID_JOYPAD_UP,
      RETRO_DEVICE_ID_JOYPAD_DOWN,
      RETRO_DEVICE_ID_JOYPAD_LEFT,
      RETRO_DEVICE_ID_JOYPAD_RIGHT,
      RETRO_DEVICE_ID_JOYPAD_A,
      RETRO_DEVICE_ID_JOYPAD_X,
      RETRO_DEVICE_ID_JOYPAD_L,
      RETRO_DEVICE_ID_JOYPAD_R,
   };

   for (j = 0; j < MAX_PLAYERS; j++)
   {
      unsigned i;
      uint16_t input_state = 0;
      for (i = 0; i < MAX_BUTTONS; i++)
         input_state |= input_state_cb(j, RETRO_DEVICE_JOYPAD, 0, map[i]) ? (1 << i) : 0;

#ifdef MSB_FIRST
      union {
         uint8_t b[2];
         uint16_t s;
      } u;
      u.s = input_buf[j];
      input_buf[j] = u.b[0] | u.b[1] << 8;
#else
      input_buf[j][0] = (input_state >> 0) & 0xff;
      input_buf[j][1] = (input_state >> 8) & 0xff;
#endif
   }
}

static uint64_t video_frames, audio_frames;

void retro_run(void)
{
   MDFNGI *curgame = game;

   input_poll_cb();

   update_input();

   static int16_t sound_buf[0x10000];
   static MDFN_Rect rects[FB_MAX_HEIGHT];
   rects[0].w = ~0;

   EmulateSpecStruct spec = {0};
   spec.surface = surf;
   spec.SoundRate = 44100;
   spec.SoundBuf = sound_buf;
   spec.LineWidths = rects;
   spec.SoundBufMaxSize = sizeof(sound_buf) / 2;
   spec.SoundVolume = 1.0;
   spec.soundmultiplier = 1.0;
   spec.SoundBufSize = 0;
   spec.VideoFormatChanged = false;
   spec.SoundFormatChanged = false;

   if (memcmp(&last_pixel_format, &spec.surface->format, sizeof(MDFN_PixelFormat)))
   {
      spec.VideoFormatChanged = TRUE;

      last_pixel_format = spec.surface->format;
   }

   if (spec.SoundRate != last_sound_rate)
   {
      spec.SoundFormatChanged = true;
      last_sound_rate = spec.SoundRate;
   }

   curgame->Emulate(&spec);

#ifdef NEED_DEINTERLACER
   if (spec.InterlaceOn)
   {
      if (!PrevInterlaced)
         deint.ClearState();

      deint.Process(spec.surface, spec.DisplayRect, spec.LineWidths, spec.InterlaceField);

      PrevInterlaced = true;

      spec.InterlaceOn = false;
      spec.InterlaceField = 0;
   }
   else
      PrevInterlaced = false;
#endif

   int16 *const SoundBuf = spec.SoundBuf + spec.SoundBufSizeALMS * curgame->soundchan;
   int32 SoundBufSize = spec.SoundBufSize - spec.SoundBufSizeALMS;
   const int32 SoundBufMaxSize = spec.SoundBufMaxSize - spec.SoundBufSizeALMS;

   spec.SoundBufSize = spec.SoundBufSizeALMS + SoundBufSize;

   unsigned width  = spec.DisplayRect.w;
   unsigned height = spec.DisplayRect.h;

#if defined(WANT_32BPP)
   const uint32_t *pix = surf->pixels;
   video_cb(pix, width, height, FB_WIDTH << 2);
#elif defined(WANT_16BPP)
   const uint16_t *pix = surf->pixels16;
   video_cb(pix, width, height, FB_WIDTH << 1);
#endif

   video_frames++;
   audio_frames += spec.SoundBufSize;

   audio_batch_cb(spec.SoundBuf, spec.SoundBufSize);

   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = MEDNAFEN_CORE_NAME;
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version  = MEDNAFEN_CORE_VERSION GIT_VERSION;
   info->need_fullpath    = true;
   info->valid_extensions = MEDNAFEN_CORE_EXTENSIONS;
   info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps            = MEDNAFEN_CORE_TIMING_FPS;
   info->timing.sample_rate    = 44100;
   info->geometry.base_width   = MEDNAFEN_CORE_GEOMETRY_BASE_W;
   info->geometry.base_height  = MEDNAFEN_CORE_GEOMETRY_BASE_H;
   info->geometry.max_width    = MEDNAFEN_CORE_GEOMETRY_MAX_W;
   info->geometry.max_height   = MEDNAFEN_CORE_GEOMETRY_MAX_H;
   info->geometry.aspect_ratio = MEDNAFEN_CORE_GEOMETRY_ASPECT_RATIO;
}

void retro_deinit(void)
{
   if (surf)
   {
#if defined(WANT_32BPP)
      if (surf->pixels)
         free(surf->pixels);
#elif defined(WANT_16BPP)
      if (surf->pixels16)
         free(surf->pixels16);
#endif
      delete surf;
   }
   surf = NULL;

   if (log_cb)
   {
      log_cb(RETRO_LOG_INFO, "[%s]: Samples / Frame: %.5f\n",
            MEDNAFEN_CORE_NAME, (double)audio_frames / video_frames);
      log_cb(RETRO_LOG_INFO, "[%s]: Estimated FPS: %.5f\n",
            MEDNAFEN_CORE_NAME, (double)video_frames * 44100 / audio_frames);
   }
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC; // FIXME: Regions for other cores.
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned in_port, unsigned device)
{
   MDFNGI *currgame = (MDFNGI*)game;

   if (!currgame)
      return;
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

static size_t serialize_size;

size_t retro_serialize_size(void)
{
   StateMem st;

   st.data           = NULL;
   st.loc            = 0;
   st.len            = 0;
   st.malloced       = 0;
   st.initial_malloc = 0;

   if (!MDFNSS_SaveSM(&st, 0, 0, NULL, NULL, NULL))
      return 0;

   free(st.data);
   return serialize_size = st.len;
}

bool retro_serialize(void *data, size_t size)
{
   StateMem st;
   bool ret          = false;
   uint8_t *_dat     = (uint8_t*)malloc(size);

   if (!_dat)
      return false;

   st.data           = _dat;
   st.loc            = 0;
   st.len            = 0;
   st.malloced       = size;
   st.initial_malloc = 0;

   ret = MDFNSS_SaveSM(&st, 0, 0, NULL, NULL, NULL);

   memcpy(data,st.data,size);
   free(st.data);
   return ret;
}

bool retro_unserialize(const void *data, size_t size)
{
   StateMem st;

   st.data           = (uint8_t*)data;
   st.loc            = 0;
   st.len            = size;
   st.malloced       = 0;
   st.initial_malloc = 0;

   return MDFNSS_LoadSM(&st, 0, 0);
}

void *retro_get_memory_data(unsigned type)
{
   if ( type == RETRO_MEMORY_SYSTEM_RAM )
      return SNES::memory::wram.data();
   return NULL;
}
size_t retro_get_memory_size(unsigned type)
{
   if ( type == RETRO_MEMORY_SYSTEM_RAM )
      return SNES::memory::wram.size();
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned, bool, const char *)
{}

#ifdef _WIN32
static void sanitize_path(std::string &path)
{
   size_t size = path.size();
   for (size_t i = 0; i < size; i++)
      if (path[i] == '/')
         path[i] = '\\';
}
#endif

// Use a simpler approach to make sure that things go right for libretro.
std::string MDFN_MakeFName(MakeFName_Type type, int id1, const char *cd1)
{
   char slash;
#ifdef _WIN32
   slash = '\\';
#else
   slash = '/';
#endif
   std::string ret;
   switch (type)
   {
      case MDFNMKF_SAV:
         ret = retro_save_directory +slash + retro_base_name +
            std::string(".") + std::string(cd1);
         break;
      case MDFNMKF_FIRMWARE:
         ret = retro_base_directory + slash + std::string(cd1);
#ifdef _WIN32
   sanitize_path(ret); // Because Windows path handling is mongoloid.
#endif
         break;
      default:	  
         break;
   }

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "MDFN_MakeFName: %s\n", ret.c_str());
   return ret;
}

void MDFND_DispMessage(unsigned char *str)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "%s\n", str);
}

void MDFND_Message(const char *str)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "%s\n", str);
}

void MDFND_MidSync(const EmulateSpecStruct *)
{}

void MDFN_MidLineUpdate(EmulateSpecStruct *espec, int y)
{
 //MDFND_MidLineUpdate(espec, y);
}

void MDFND_PrintError(const char* err)
{
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "%s\n", err);
}
