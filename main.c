#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
// #include "hardware/i2c.h"
#include "hardware/timer.h"


#include "hardware/vreg.h"
#include <hardware/structs/qmi.h>
#include <hardware/structs/xip.h>


#include "./Game/test.h"
#include "./Game/Castle.h"
#include "./Game/pico9918.h"
#include "./Game/Soukoban.h"
#include "./Game/Yamato.h"
#include "./Game/retropipe.h"

#include "Z80pinout.h"
#include "VDP_int_Z80.pio.h"

/* JOYPAD BUTTONS

Button      1REG        2REG
--------------------------------

*/

// NES-compatible gamepad (shift register) pins
#define PIN_NES_JOYPAD_CLOCK 42
#define PIN_NES_JOYPAD_LATCH 43
#define PIN_NES_JOYPAD_DATA1  44
#define PIN_NES_JOYPAD_DATA2  45
#define NES_JOYPAD_SHIFT_COUNT 8

#define i2c_PCF8575_port (i2c0)
#define CLOCK_I2C_400kHz (400)
#define I2C_PCF8575_ADDR (0x20)
#define PICO_I2C_PCF8575_SDA_PIN (0)
#define PICO_I2C_PCF8575_SCL_PIN (1)

//RESET
#define RESET_PIN      34
#define RESET_MASK (1ULL << RESET_PIN)

// /VDP_RD 
#define VDP_RD      38
#define VDP_RD_MASK (1ULL << VDP_RD)

// /VDP_WR 
#define VDP_WR      39
#define VDP_WR_MASK (1ULL << VDP_WR)

// /AUD_W
#define AUD_W       41
#define AUD_W_MASK (1ULL << AUD_W)

#define VDP_INT_PIO     pio0
#define VDP_INT_SM      0

#define MAP_SEL (0b00000000)
#define PSG_WR  (0b01000000)
#define VDP     (0b10000000)
#define IO_SEL  (0b11000000)

uint8_t __aligned(4) sega_cartrige[0xC000];
uint8_t __aligned(4) sega_ram[0x4000];

uint8_t upper_bank = 1;
uint8_t lower_bank = 0;
bool page0 = false;
bool page1 = false;

volatile uint8_t reg_1 = 0x01; //
volatile uint8_t reg_2 = 0x02; //

static uint8_t sega_joysticks [2] = {0xff,0xff};
volatile uint8_t data_joysticks[2];
volatile bool start_read_joypad = true;

bool hold_sw ;      //freeze appl


void inInit(uint gpio) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);
}

void outInit(uint gpio,bool set_up_out) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_OUT);
    gpio_put(gpio, set_up_out);
}

// Read NES-compatible gamepad via latch/clocked serial interface.
// Returns an active-high button mask after converting from active-low bits.
uint16_t read_joystick_data() {
    uint8_t serial_data1 = 0;
    uint8_t serial_data2 = 0;
    uint16_t out_joypads;
    // Latch current button states into the controller's shift register
    gpio_put(PIN_NES_JOYPAD_LATCH, 1);
    busy_wait_us(20);
    gpio_put(PIN_NES_JOYPAD_LATCH, 0);
    busy_wait_us(20);

    // Shift in 8 bits (standard NES controller protocol)
    for (int i = 0; i < NES_JOYPAD_SHIFT_COUNT; i++) {
        gpio_put(PIN_NES_JOYPAD_CLOCK, 0);
        busy_wait_us(20);
        serial_data1 <<= 1;
        serial_data1 |= gpio_get(PIN_NES_JOYPAD_DATA1);
        serial_data2 <<= 1;
        serial_data2 |= gpio_get(PIN_NES_JOYPAD_DATA2);        
        gpio_put(PIN_NES_JOYPAD_CLOCK, 1);
        busy_wait_us(20);
    }
    out_joypads = (serial_data2<<8) | serial_data1;
    return out_joypads ;
};

//convert NES to COLECO joypad
#define     NES_UP              (1 << 3)
#define     NES_DOWN            (1 << 2)
#define     NES_LEFT            (1 << 1)
#define     NES_RIGHT           (1 << 0)
#define     NES_A               (1 << 7)
#define     NES_B               (1 << 6)
#define     NES_SELECT          (1 << 5)
#define     NES_START           (1 << 4)

#define     UP_BUTTON1        0b11111110
#define     RIGHT_BUTTON1     0b11110111
#define     DOWN_BUTTON1      0b11111101
#define     LEFT_BUTTON1      0b11111011
#define     SHOTL_BUTTON1     0b11101111
#define     SHOTR_BUTTON1     0b11011111

#define     UP_BUTTON2        0b10111111
#define     RIGHT_BUTTON2     0b11111101
#define     DOWN_BUTTON2      0b01111111
#define     LEFT_BUTTON2      0b11111110
#define     SHOTL_BUTTON2     0b11111011
#define     SHOTR_BUTTON2     0b11110111

void convert_nes_to_sega_joypad(uint16_t nes_joy_data){

    // Convert active-low layout from controller: pressed -> clear bit.
    uint8_t nes_joy_data1 = (uint8_t)(nes_joy_data & 0x00ff);
    uint8_t nes_joy_data2 = (uint8_t)((nes_joy_data & 0xff00) >> 8);
    sega_joysticks[0] = 0xFF;
    sega_joysticks[1] = 0xFF;
// first joypad
    if ((nes_joy_data1 & NES_UP) == 0) {sega_joysticks[0] &= UP_BUTTON1;}
    if ((nes_joy_data1 & NES_DOWN) == 0) {sega_joysticks[0] &= DOWN_BUTTON1;}
    if ((nes_joy_data1 & NES_LEFT) == 0) {sega_joysticks[0] &= LEFT_BUTTON1;}
    if ((nes_joy_data1 & NES_RIGHT) == 0) {sega_joysticks[0] &= RIGHT_BUTTON1;}
    if ((nes_joy_data1 & NES_B) == 0) {sega_joysticks[0] &= SHOTL_BUTTON1;}
    if ((nes_joy_data1 & NES_A) == 0) sega_joysticks[0] &= SHOTR_BUTTON1;
 
    if ((nes_joy_data1 & NES_START) == 0) {  hold_sw = 1; }

// second joypad
    if ((nes_joy_data2 & NES_UP) == 0) {sega_joysticks[0] &= UP_BUTTON2;}
    if ((nes_joy_data2 & NES_DOWN) == 0) {sega_joysticks[0] &= DOWN_BUTTON2;}
    if ((nes_joy_data2 & NES_LEFT) == 0) {sega_joysticks[1] &= LEFT_BUTTON2;}
    if ((nes_joy_data2 & NES_RIGHT) == 0) {sega_joysticks[1] &= RIGHT_BUTTON2;}
    if ((nes_joy_data2 & NES_B) == 0) {sega_joysticks[1] &= SHOTL_BUTTON2;}
    if ((nes_joy_data2 & NES_A) == 0) sega_joysticks[1] &= SHOTR_BUTTON2;
}
// init pins 
void Z80_pin_setup() {
    inInit(MREQ_PIN);
    inInit(WR_PIN);
    inInit(RD_PIN);
    inInit(IORQ_PIN);
    // inInit(M1_PIN);
    
    inInit(PIN_NES_JOYPAD_DATA1);
    inInit(PIN_NES_JOYPAD_DATA2);    

    // inInit(ENCODER_IN_A);
    // inInit(ENCODER_IN_B);

    outInit(RESET_PIN,0);
    outInit(WAIT_PIN, 1);
    outInit(NMI_PIN, 1);
    // outInit(INT_PIN, 1);
    outInit(VDP_RD,1);
    outInit(VDP_WR,1);
    outInit(AUD_W,1);
    outInit(PIN_NES_JOYPAD_CLOCK,1);
    outInit(PIN_NES_JOYPAD_LATCH,1);
}

void VDP_INT_init() {
    PIO piopio = VDP_INT_PIO;
    uint smsm  = VDP_INT_SM;
    pio_set_gpio_base(piopio, 16);
    const uint pio_offset = pio_add_program(piopio, &vdpintz80_program);
    vdpintz80_program_init(piopio, smsm, pio_offset);
    pio_sm_set_enabled(piopio, smsm, true);
}

// Z80 write-side address decode: ioport,.
__always_inline static inline void write_sega_io(const uint8_t portIO, const uint8_t value) {

    switch (portIO & 0xC0)
        {
        case MAP_SEL:    
                    { upper_bank = (value & 0x03);}           // page            
        case PSG_WR:                                          // CS PSG
                   {
                        gpio_put(AUD_W,0);
                   } 
        break;    
        case VDP:                                          // CS VDP_WR
                    {
                        gpio_put(VDP_WR,0);
                    }
        break;
        // case 0x00C0:                   
        default:
            break;
        }            

}

// Z80 write-side address decode: ioport,.
__always_inline static inline void read_sega_io(const uint8_t portIO) {

    switch (portIO & 0xC0)
        {                 
        case VDP:                
            {gpio_put(VDP_RD,0);}
        break;   
        case IO_SEL:
            {              
                uint8_t temp = 0xFF;
                if((portIO&0xE1) == 0xC1){                   
                    temp = data_joysticks[1];                       //  joypad 2
                } 
                else if((portIO&0xE1) == 0xC0){                   
                    temp = data_joysticks[0];                       //  joypad 1
                }
                const uint32_t data = ((uint32_t)temp) << 22 ; 
                gpio_put_masked(DATA_MASK,data);           
                gpio_set_dir_out_masked(DATA_MASK);
                start_read_joypad = true;                               
            }
        break;                    
        default:
        break;
        }            

}

// Z80 read-side address decode: RAM, ROM banks,and BIOS.
__always_inline static inline uint8_t read_sega_memory(const uint16_t address) {

        switch (address & 0xC000)
    {
        case 0x0000:
        case 0x4000:
        case 0x8000:            
            {
                return sega_cartrige[address];
            }             
        case 0xC000: 
            {
                return sega_ram[address & 0x3fff];
            }              
    // Default open-bus value
    default:
    return 0xFF;
    }
}

__always_inline static inline uint8_t write_sega_memory(const uint16_t address,const uint8_t value){
    switch (address & 0xC000)
    {
        case 0x8000:            
            {
                return sega_cartrige[address] = value;
            }

        case 0xC000:
            {
                return sega_ram[address & 0x3fff] = value;
            }
        default:
        break;
    }
}

void __time_critical_func(Z80_loop)() {
    while(true){
            gpio_set_dir_in_masked(DATA_MASK);

        //------------memory ok-----------------

        while(!(gpio_get_all64() & MREQ_MASK)) {                                    // memory read write
                uint16_t address = (uint16_t)((gpio_get_all() >> 6) & 0x0000ffff);  
                while(!(gpio_get_all() & READ_MASK)) {                                  //  read from         
                    const uint32_t data = read_sega_memory(address) << 22;
                    gpio_put_masked(DATA_MASK,data);
                    gpio_set_dir_out_masked(DATA_MASK);
                }

            gpio_set_dir_in_masked(DATA_MASK); 
                            
                while(!(gpio_get_all() & WRITE_MASK)) {                                 // write to ram                            
                        // : SEGA RAM
                    uint8_t data_to_ram = (uint8_t)((gpio_get_all() >>22) & 0x000000FF);
                    write_sega_memory(address,data_to_ram);                 
                }         
        }
    
    // io port
        while(!(gpio_get_all64() & IORQ_MASK)) {
            uint16_t address = (gpio_get_all() >> 6) ;              // mask io port address
            uint8_t portIO = (uint8_t)(0x00FF & address);

    // read from IO             
            while(!(gpio_get_all() & READ_MASK)) {                              // read from IO
                read_sega_io(portIO);                                
                }
    // write to IO        
            while(!(gpio_get_all64() & WRITE_MASK)) {                              // write to IO

                uint8_t datawritetoio = (uint8_t)((gpio_get_all() >>22)&0x000000ff);
                write_sega_io(portIO,datawritetoio);
                }
            gpio_set_dir_in_masked(DATA_MASK);

        }
        gpio_put(VDP_WR,1);
        gpio_put(VDP_RD,1);
        gpio_put(AUD_W,1);

    }
}

// main------------------------------------------

    int main() {


    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);

    qmi_hw->m[0].timing = 0x60007304; // 4x FLASH divisor

    sleep_ms(100);
    // if (!set_sys_clock_hz(CPU_FREQ_MHZ * MHZ, 0) ) {
        set_sys_clock_hz(315 * MHZ, 1); // fallback to failsafe clocks
    // }

    stdio_init_all();

    

    // clock for CPU
 

    // Initialize pins
    gpio_init_mask(ADDR_MASK | DATA_MASK);
    gpio_set_dir_in_masked(ADDR_MASK | DATA_MASK);
    Z80_pin_setup();

    // Initialize select out interrupt from VDP to Z80

    VDP_INT_init();

    // memcpy(sega_cartrige, test_rom,test_rom_len);
    memcpy(sega_cartrige, Castle_sg, Castle_sg_len);
    // memcpy(sega_cartrige, pico9918_sg, pico9918_sg_len);
    // memcpy(sega_cartrige, Soukoban_sg, Soukoban_sg_len);
    // memcpy(sega_cartrige, Yamato_sg, Yamato_sg_len);
    // memcpy(sega_cartrige, retropipe_sg, retropipe_sg_len);

    memset(sega_ram, 0x00, 0x4000);

    multicore_launch_core1(Z80_loop);

// reset
    gpio_put(RESET_PIN,1);    busy_wait_ms(200);
    gpio_put(RESET_PIN,0);    busy_wait_ms(200);    
    gpio_put(RESET_PIN,1); 
    

    int timer_ms =us_to_ms(time_us_32());


    while (true) {

        if(start_read_joypad){            
            convert_nes_to_sega_joypad(read_joystick_data());
            for (size_t i = 0; i < 2; i++){
                data_joysticks[i] = sega_joysticks[i];
            }
            start_read_joypad = false;
            timer_ms =us_to_ms(time_us_32());                                               
        } 

// joystick polling call every 100 ms if interrupt polling does not work
        if((us_to_ms(time_us_32()) - timer_ms) > 100){
            start_read_joypad = true; 
        }
// 300 ms NMI pulse for pause        
       if(hold_sw){
        gpio_put(NMI_PIN,!hold_sw);
        hold_sw = 0;
        busy_wait_ms(300);
        gpio_put(NMI_PIN,!hold_sw);        
       }

    }//while
}//main
