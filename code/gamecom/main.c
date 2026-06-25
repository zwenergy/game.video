#include <stdio.h>

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/bus_ctrl.h"

#include <math.h>
#include <string.h>

// PIOs.
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/adc.h"
#include "captureVideo.pio.h"
#include "frameStart.pio.h"
#include "frameStartNotifier.pio.h"

#define AUDIO_PIN 27
#define ADC_CHANNEL 1
#define AUDIO_SAMPLES 1024

#ifdef VIDEO_MODE_1280x720
#define FRAME_WIDTH 1280
#define FRAME_HEIGHT 720
#define BOX_SIZE 64
#define SCALE 4
#define SCALE4
#else
#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define BOX_SIZE 32
#define SCALE 2
#endif
#define BG_COLOR 0x0010  // Dark blue (RGB565)
#define BOX_COLOR 0xFFE0 // Yellow (RGB565)

// GAME.COM Defs.
#define DAT0 0
#define DAT1 1
#define DAT2 2
#define DAT3 3
#define DCLK 4
#define VSYNC 5

#define GAMECOM_WIDTH 200
#define GAMECOM_HEIGHT 160
#define GAMECOM_RAWBUFFER_SIZE ( ( GAMECOM_WIDTH * GAMECOM_HEIGHT ) / 32 )
#define FRAMEBLEND 4

#define VIEWPORT_X ( ( FRAME_WIDTH - ( GAMECOM_WIDTH * SCALE ) ) / 2 )
#define VIEWPORT_Y ( ( FRAME_HEIGHT - ( GAMECOM_HEIGHT * SCALE ) ) / 2 )

#define VSYNC_WAIT_CNT ( ( ( GAMECOM_WIDTH * GAMECOM_HEIGHT ) / 4 ) - 40 )

volatile int audioDMAChanDat;
int audio_frame_counter = 0;

uint16_t audioBuffer[ CAPTURE_SAMPLES ] __attribute__((section(".sramdma"))) __attribute__((aligned(2048)));;

unsigned bufferReadCnt __attribute__((section(".sramdma"))) = 0;
unsigned bufferWriteCnt __attribute__((section(".sramdma"))) = 0;
unsigned fullBuffer __attribute__((section(".sramdma"))) = 0;

// Filter.
float previous_sample = 0.0f;
float alpha = 0.5f; // Lower = more muffled/smoother, Higher = more noise/clearer

static void __not_in_flash_func( pushAudioSamples )() {
  bufferWriteCnt =  ( ( dma_hw->ch[ audioDMAChanDat ].write_addr - ( (uint32_t) audioBuffer ) ) / 2 );
  unsigned ptrDist = ( bufferWriteCnt - bufferReadCnt ) % AUDIO_SAMPLES;
  
  // Keep the audio queue fed.
  while ( hstx_di_queue_get_level() < 200 && ptrDist >= 8 ) {
    audio_sample_t samples[ 4 ];
    for ( int i = 0; i < 4; i++ ) {
      // Unsigned 12b sample centered around 2^11     
      int16_t s = audioBuffer[ bufferReadCnt++ ];
      // Filter it.
      float current_sample = (float) s;
      float filtered = ( alpha * current_sample ) + ( ( 1.0f - alpha ) * previous_sample );
    
      // Store for the next iteration
      previous_sample = filtered;
    
      // Cast back to integer
      s = (int16_t) filtered;
      
      if ( bufferReadCnt == AUDIO_SAMPLES ) {
        bufferReadCnt = 0;
      }
      
      // Center
      s = s - ( 1 << 11 );
      
      if ( s < NOISE_GATE && s > -NOISE_GATE ) {
        s = 0;
      }
      
      // Increase volume.
      s = s << 3;
      samples[i].left = s;
      samples[i].right = s;
    }

    // Create audio packet.
    hstx_packet_t packet;
    audio_frame_counter = hstx_packet_set_audio_samples( &packet, samples, 4, audio_frame_counter );

    hstx_data_island_t island;
    hstx_encode_data_island( &island, &packet, false, DI_HSYNC_ACTIVE );
    hstx_di_queue_push( &island );
    
    // Update distance.
    bufferWriteCnt =  ( ( dma_hw->ch[ audioDMAChanDat ].write_addr - ( (uint32_t) audioBuffer ) ) / 2 );
    ptrDist = ( bufferWriteCnt - bufferReadCnt ) % AUDIO_SAMPLES;
  }
  
}

uint32_t rawFB[ GAMECOM_RAWBUFFER_SIZE * 2 ] __attribute__((section(".sramdma")));
PIO notifierPIO;
uint notifierSM;

unsigned int tmpSM;

void __not_in_flash_func( doPIOStuff )() {
  printf( "do pio stuff\n" );
  // Set up PIOs.
  
  // Video capture PIO.
  PIO pio = pio0;
  uint offset_captureVideo = pio_add_program( pio, &captureVideo_program );
  uint sm_captureVideo = pio_claim_unused_sm( pio, true );
  
  // Frame start reset PIO.
  uint sm_frameStart = pio_claim_unused_sm( pio, true );
  uint offset_frameStart = pio_add_program( pio, &frameStart_program );
  tmpSM = sm_frameStart;
  
  // Frame start notifier.
  uint sm_frameStartNotifier = pio_claim_unused_sm( pio, true );
  uint offset_frameStartNotifier = pio_add_program( pio, &frameStartNotifier_program );
  notifierPIO = pio;
  notifierSM = sm_frameStartNotifier;
  
  printf( "Created pios\n" );

  
  // DMA to copy pixels to FB.
  int fbFill_dma = 3;
  
  dma_channel_config c = dma_channel_get_default_config( fbFill_dma );
  channel_config_set_transfer_data_size( &c, DMA_SIZE_32 );
  channel_config_set_read_increment( &c, false );
  channel_config_set_write_increment( &c, true );
  channel_config_set_dreq( &c, pio_get_dreq( pio, sm_captureVideo, false) );

  dma_channel_configure(
    fbFill_dma,
    &c,
    rawFB, // Write to FB.
    &pio->rxf[ sm_captureVideo ],  // Read from captureVideo RX FIFO
    dma_encode_endless_transfer_count(),        // run inf.
    false                                       // Don't start yet
  );
  
  // DMA to reset fbFill DMA.
  int fbReset_dma = 4;
  c = dma_channel_get_default_config( fbReset_dma );
  channel_config_set_transfer_data_size( &c, DMA_SIZE_32 );
  channel_config_set_read_increment( &c, false );
  channel_config_set_write_increment( &c, false );
  channel_config_set_dreq( &c, pio_get_dreq( pio, sm_frameStart, false) );

  dma_channel_configure(
    fbReset_dma,
    &c,
    &dma_hw->ch[ fbFill_dma ].write_addr, // Write to fbFill DMA's write address.
    &pio->rxf[ sm_frameStart ],  // Read from frameStart RX FIFO
    dma_encode_endless_transfer_count(),        // run inf.
    false                                       // Don't start yet
  );
  
  printf( "Created DMAs\n" );
  
  // Start the DMAs.
  dma_start_channel_mask( 1u << fbFill_dma );
  dma_start_channel_mask( 1u << fbReset_dma );
  
  // Start the SMs.
  frameStart_program_init( pio, sm_frameStart, offset_frameStart );
  frameStartNotifier_program_init( pio, sm_frameStartNotifier, offset_frameStartNotifier );

  // Put the FB addr.
  pio_sm_put( pio, sm_frameStart, (uint32_t) rawFB );
  
  // Put the wait pixels.
  pio_sm_put( pio, sm_frameStart, VSYNC_WAIT_CNT );
  pio_sm_put( pio, sm_frameStartNotifier, VSYNC_WAIT_CNT );
  
  captureVideo_program_init( pio, sm_captureVideo, offset_captureVideo, DAT0 );
}

uint32_t __not_in_flash_func( expandTo16bPixel )( uint32_t pxl ) {
  uint32_t c;
  
  if ( pxl == 0b00000100 ) {
    c = 0b1111111111111111;
  } else if ( pxl >= 0b00000010 ) {
    if ( pxl == 0b00000010 ) {
      c = 0b1000010000010000;
    } else {
      c = 0b1011110111110111;
    }
  } else {
    if ( pxl == 0b00000001 ) {
      c = 0b0100001000001000;
    } else {
      c = 0b0000000000000000;
    }
  }
  
  return c;
}

uint32_t __not_in_flash_func( expandAndCombine )( uint32_t pxl0, uint32_t pxl1 ) {
  return ( ( ( expandTo16bPixel( pxl1 )  ) << 16 ) | expandTo16bPixel( pxl0 ) );  
};

uint32_t localRawFB[ GAMECOM_RAWBUFFER_SIZE * FRAMEBLEND ];
uint32_t curFB[ GAMECOM_HEIGHT * GAMECOM_WIDTH ];
#ifdef SCALE4
uint32_t expanded16bFB[ ( GAMECOM_HEIGHT * GAMECOM_WIDTH * 2 ) / 2 ];
#else
uint32_t expanded16bFB[ ( GAMECOM_HEIGHT * GAMECOM_WIDTH * SCALE ) / 2 ];
#endif

unsigned frameCnt = 0;
void __not_in_flash_func( processCapture )() {
  
  // Wait until a new frame was captured.
  if ( pio_sm_is_rx_fifo_empty( notifierPIO, notifierSM ) ) {
    return;
  }
  
  // Copy the data.
  memcpy( localRawFB + (GAMECOM_RAWBUFFER_SIZE * frameCnt), rawFB, 
    GAMECOM_RAWBUFFER_SIZE * 4 );
    
  // Get the dummy to empty the FIFO.    
  pio_sm_get( notifierPIO, notifierSM );
  
  if ( frameCnt == ( FRAMEBLEND - 1 ) ) {
    frameCnt = 0;
  } else {
    ++frameCnt;
  }
  
  // Do the actual processing to create the current frame buffer.
  for ( unsigned blendCnt = 0; blendCnt < FRAMEBLEND; ++blendCnt ) {
    unsigned curX = 0;
    unsigned curY = 0;
    unsigned rotCnt = 0;
    for ( unsigned sampleCnt = 0; sampleCnt < GAMECOM_RAWBUFFER_SIZE; ++sampleCnt ) {      
      // Get the current sample.
      uint32_t curSample = ~( localRawFB[ sampleCnt + ( blendCnt * GAMECOM_RAWBUFFER_SIZE ) ] );
      // Each sample contains 32 pixels, grouped in 4 lines. Go through them.
      for ( unsigned groupCnt = 0; groupCnt < 8; ++groupCnt ) {
        
        uint32_t curGroup = ( curSample >> ( groupCnt * 4 ) );
        uint32_t dat0 = ( curGroup & 0b000001 ) >> 0;
        uint32_t dat1 = ( curGroup & 0b000010 ) >> 1;
        uint32_t dat2 = ( curGroup & 0b000100 ) >> 2;
        uint32_t dat3 = ( curGroup & 0b001000 ) >> 3;
        
        if ( blendCnt == 0 ) {
          curFB[ curX + ( ( curY + 0 ) * GAMECOM_WIDTH ) ] = dat0;
          curFB[ curX + ( ( curY + 1 ) * GAMECOM_WIDTH ) ] = dat1;
          curFB[ curX + ( ( curY + 2 ) * GAMECOM_WIDTH ) ] = dat2;
          curFB[ curX + ( ( curY + 3 ) * GAMECOM_WIDTH ) ] = dat3;
        } else {
          curFB[ curX + ( ( curY + 0 ) * GAMECOM_WIDTH ) ] += dat0;
          curFB[ curX + ( ( curY + 1 ) * GAMECOM_WIDTH ) ] += dat1;
          curFB[ curX + ( ( curY + 2 ) * GAMECOM_WIDTH ) ] += dat2;
          curFB[ curX + ( ( curY + 3 ) * GAMECOM_WIDTH ) ] += dat3;
        }
        
        if ( rotCnt < 39 ) {
          rotCnt += 1;
          curY += 4;
        } else {
          rotCnt = 0;
          curY = 0;
          curX += 1;
        }
      }
    }
  }
  
  
  // Expand values to RGB 565.
  if ( SCALE == 1 ) {
    for ( unsigned y = 0; y < GAMECOM_HEIGHT; ++y ) {
      for ( unsigned x = 0; x < GAMECOM_WIDTH; x += 2  ) {
        expanded16bFB[ ( x / 2 ) + ( y * ( GAMECOM_WIDTH / 2 ) ) ] = 
          expandAndCombine( curFB[ x + ( y * GAMECOM_WIDTH ) ], 
                            curFB[ x + 1 + ( y * GAMECOM_WIDTH ) ] );
      }
    }
  } else if ( SCALE == 2 ) {
    // SCALE = 2
    for ( unsigned y = 0; y < GAMECOM_HEIGHT; ++y ) {
      for ( unsigned x = 0; x < GAMECOM_WIDTH; x += 1  ) {
        uint32_t curPxl = curFB[ x + ( y * GAMECOM_WIDTH ) ];
        expanded16bFB[ ( x + ( y * ( GAMECOM_WIDTH ) ) ) ] = 
          expandAndCombine( curPxl, curPxl );
      }
    }
  } else if ( SCALE == 4 ) {
    
    unsigned expCnt = 0;
    for ( unsigned y = 0; y < GAMECOM_HEIGHT; ++y ) {
      // Also do some audio stuff in beetween.
      for ( unsigned x = 0; x < GAMECOM_WIDTH; x += 1  ) {
        uint32_t curPxl = curFB[ x + ( y * GAMECOM_WIDTH ) ];
        
        expanded16bFB[ expCnt++ ] = 
          expandAndCombine( curPxl, curPxl );
      }
    }
  }
}

// ============================================================================
// Scanline Callback (runs on Core 1)
// ============================================================================

static void __scratch_x("") scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
  (void) v_scanline;

  int fb_line = active_line;

  uint32_t bg = BG_COLOR | (BG_COLOR << 16);
      
  // Viewport lines?
  if ( fb_line >= VIEWPORT_Y && fb_line < ( VIEWPORT_Y + ( GAMECOM_HEIGHT * SCALE ) ) ) {
    int i = 0;
    // Region 1: before viewport
    // Note: iterating by 2 pixels at a time (1 uint32_t)
    for (; i < ( VIEWPORT_X / 2 ); i++) {
        dst[i] = bg;
    }
    
    // Region 2: viewport
    #ifdef SCALE4
    const uint32_t w = ( GAMECOM_WIDTH / 2 ) * 2;
    int n = ( ( fb_line - VIEWPORT_Y ) / SCALE ) * ( w );
    // Note: iterating by 2 pixels at a time (1 uint32_t)
    for (; i < ( VIEWPORT_X / 2 ) + (w*2); i++) {
        dst[i++] = expanded16bFB[ n ];
        dst[i] = expanded16bFB[ n++ ];
    }
    #else
    const uint32_t w = ( GAMECOM_WIDTH / 2 ) * SCALE;
    int n = ( ( fb_line - VIEWPORT_Y ) / SCALE ) * ( w );
    // Note: iterating by 2 pixels at a time (1 uint32_t)
    for (; i < ( VIEWPORT_X / 2 ) + w; i++) {
        dst[i] = expanded16bFB[ n++ ];
    }
    #endif
    
    // Region 3: after viewport.
    for (; i < FRAME_WIDTH / 2; i++) {
          dst[i] = bg;
      }
    
  } else {
    // Background.
    for (int i = 0; i < FRAME_WIDTH / 2; i++) {
      dst[i] = bg;
    }
  }
}

void __not_in_flash_func( videoStuff )() {
  while ( 1 ) {
    // check for game.com frames.
    processCapture();
    // Do audio.
    pushAudioSamples();
  }
}
  

volatile uint32_t audioBufferAddr = (uint32_t) audioBuffer;
int main(void)
{ 
#ifdef VIDEO_MODE_1280x720
    // 720p60: 372 MHz at 1.3V. Closest achievable to 371.25 MHz with 12 MHz XOSC
    // (0.2% high -> 74.4 MHz pixel clock, within HDMI tolerance for 720p60).
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
    set_sys_clock_khz(372000, true);
#else
    sleep_ms(2);
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(2);
    set_sys_clock_khz(126000 * 2, true);
#endif
    
    sleep_ms(10);
    stdio_init_all();

    // Initialize LED.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    
    // Init audio ADC.
    adc_init();
    adc_gpio_init( AUDIO_PIN );
    adc_select_input( ADC_CHANNEL );

    
    adc_fifo_setup(
      true,    // Write each completed conversion to the FIFO
      true,    // Enable DMA data request (DREQ)
      1,       // IREQ asserted when at least 1 samples is present
      false,   // We don't need the error bit
      false    // Keep samples at 12-bit resolution
    );

    adc_set_clkdiv( 999.0f ); // Aiming for ~48kHz
    
    
    
    
    // Audio DMA
    audioDMAChanDat = 13;
    
    dma_channel_config cfg = dma_channel_get_default_config( audioDMAChanDat );
    channel_config_set_high_priority( &cfg, false);
    channel_config_set_transfer_data_size( &cfg, DMA_SIZE_16 );
    channel_config_set_read_increment( &cfg, false );
    channel_config_set_write_increment( &cfg, true );
    channel_config_set_dreq( &cfg, DREQ_ADC );
    channel_config_set_ring( &cfg, true, 11 );

    dma_channel_configure(
      audioDMAChanDat,
      &cfg,
      audioBuffer,
      &adc_hw->fifo,
      dma_encode_endless_transfer_count(),
      false
    );

    // Initialize HDMI output
    hstx_di_queue_init();
    video_output_init( FRAME_WIDTH, FRAME_HEIGHT );
    
    // Init game.com PIOs.
    doPIOStuff();

    // Register scanline callback
    video_output_set_scanline_callback( scanline_callback );

    // Launch Core 1 for HSTX output
    multicore_launch_core1( video_output_core1_run );
    sleep_ms( 100 );
    
    // Set the DMA write priority higher, so the writes to the HSTX over
    // the APB bridge will be prioritized.
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS;
    
    dma_channel_start( audioDMAChanDat );
    
    // Start the ADC.
    adc_run( true );
    
    videoStuff();

    return 0;
}
