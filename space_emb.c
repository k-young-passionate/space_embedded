#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include "fonts/renew_font.h"
#include "ship.h"

#define PERIPHERAL_BASE 0x3F000000UL
#define GPIO_BASE (PERIPHERAL_BASE + 0x200000)
#define SSD1306_I2C_DEV 0x3C
#define S_WIDTH 128
#define S_HEIGHT 64
#define S_PAGES (S_HEIGHT/8)
#define FONT_WIDTH 6
#define FONT_HEIGHT 1 //1 page
#define ship_WIDTH 8
#define ship_HEIGHT 8
#define ship_MOVE 4
#define NUM_FRAMES (S_WIDTH/ship_MOVE)
#define SHIP_Y_LOC 7

void set_gpio_input(void *gpio_ctr, int gpio_nr)
{
    int reg_id = gpio_nr / 10;
    int pos = gpio_nr % 10;

    uint32_t* fsel_reg = (uint32_t*) (gpio_ctr + 0x4 * reg_id);

    uint32_t fsel_val = *fsel_reg;

    uint32_t mask = 0x7 << (pos * 3);
    fsel_val = fsel_val & ~mask;

    *fsel_reg = fsel_val;
}

void set_gpio_pullup(void *gpio_ctr, int gpio_nr)
{
    int reg_id = gpio_nr / 32;
    int pos = gpio_nr % 32;

    #define GPIO_PUD_OFFSET 0x94
    #define GPIO_PUDCLK_OFFSET 0x98
    uint32_t* pud_reg = (uint32_t*) (gpio_ctr + GPIO_PUD_OFFSET);
    uint32_t* pudclk_reg = (uint32_t*) (gpio_ctr + GPIO_PUDCLK_OFFSET + 0x4 * reg_id);

    #define GPIO_PUD_PULLUP 0x2
    *pud_reg = GPIO_PUD_PULLUP;
    usleep(1);
    *pudclk_reg = (0x1 << pos);
    usleep(1);
    *pud_reg = 0;
    *pudclk_reg = 0;
}

void get_gpio_input_value(void *gpio_ctr, int gpio_nr, int *value)
{
    int reg_id = gpio_nr / 32;
    int pos = gpio_nr % 32;

    #define GPIO_LEV_OFFSET 0x34
    uint32_t* level_reg = (uint32_t*) (gpio_ctr + GPIO_LEV_OFFSET + 0x4 * reg_id);
    uint32_t level = *level_reg & (0x1 << pos);

    *value = level? 1:0;
}

void ssd1306_command(int i2c_fd,uint8_t cmd) {
    uint8_t buffer[2];
    buffer[0] = (0<<7) | (0<<6);
    buffer[1] = cmd;

    if (write(i2c_fd, buffer, 2) != 2){
        printf("i2c write failed!\n");
    }
}

void ssd1306_Init(int i2c_fd)  {
	ssd1306_command(i2c_fd,0xA8);	//Set Mux Ratio
	ssd1306_command(i2c_fd,0x3f);
	ssd1306_command(i2c_fd,0xD3);	//Set Display Offset
	ssd1306_command(i2c_fd,0x00);
	ssd1306_command(i2c_fd,0x40);	//Set Display Start Line
	ssd1306_command(i2c_fd,0xA0);	//Set Segment re-map 
									//0xA1 for vertical inversion
	ssd1306_command(i2c_fd,0xC0);	//Set COM Output Scan Direction 
									//0xC8 for horizontal inversion
	ssd1306_command(i2c_fd,0xDA);	//Set COM Pins hardware configuration
	ssd1306_command(i2c_fd,0x12);	//Manual says 0x2, but 0x12 is required
	ssd1306_command(i2c_fd,0x81);	//Set Contrast Control
	ssd1306_command(i2c_fd,0x7F);	//0:min, 0xFF:max
	ssd1306_command(i2c_fd,0xA4);	//Disable Entire Display On
	ssd1306_command(i2c_fd,0xA6);	//Set Normal Display
	ssd1306_command(i2c_fd,0xD5);	//Set OscFrequency
	ssd1306_command(i2c_fd,0x80);
	ssd1306_command(i2c_fd,0x8D);	//Enable charge pump regulator
	ssd1306_command(i2c_fd,0x14);
	ssd1306_command(i2c_fd,0xAF);	//Display ON
}

void ssd1306_data(int i2c_fd,const uint8_t*data, size_t size){
	uint8_t *buffer=(uint8_t*)malloc(size+1);
	buffer[0]=(0<<7)|(1<<6); //Co = 0 , D/C# = 1
	memcpy(buffer+1,data,size);
	if(write(i2c_fd,buffer,size+1)!=size+1){
		printf("i2c write failed!\n");
	}
	free(buffer);
}

void update_full(int i2c_fd, uint8_t *data) {
	ssd1306_command(i2c_fd,0x20); //addressing mode
	ssd1306_command(i2c_fd,0x0); //horizontal addressing mode
	ssd1306_command(i2c_fd,0x21); //set column start/end address
	ssd1306_command(i2c_fd,0);
	ssd1306_command(i2c_fd,S_WIDTH-1); 
	ssd1306_command(i2c_fd,0x22); //set page start/end address
	ssd1306_command(i2c_fd,0);
	ssd1306_command(i2c_fd,S_PAGES-1);
	ssd1306_data(i2c_fd,data,S_WIDTH*S_PAGES);
}

void update_area(int i2c_fd, const uint8_t *data, int x, int y,int x_len,int y_len) {
	ssd1306_command(i2c_fd,0x20);//addressing mode
	ssd1306_command(i2c_fd,0x0);//horizontal addressing mode
	ssd1306_command(i2c_fd,0x21);//set column start/end address
	ssd1306_command(i2c_fd,x);
	ssd1306_command(i2c_fd,x+x_len-1);
	ssd1306_command(i2c_fd,0x22);//set page start/end address
	ssd1306_command(i2c_fd,y);
	ssd1306_command(i2c_fd,y+y_len-1);
	ssd1306_data(i2c_fd,data,x_len *y_len);
}

void write_char(int i2c_fd,char c,int x,int y){
	if(c < ' ')
		c =' ';
	update_area(i2c_fd,font[c-' '],x,y,FONT_WIDTH,FONT_HEIGHT);
}

void write_str(int i2c_fd, char* str, int x, int y) {
    char c;
    while (c = *str++) {
        write_char(i2c_fd,c,x,y);
        x += FONT_WIDTH;
    }
}

int main() {
    struct ship{
        int x;
        int y;
    } player;
    int current_scene = 1;
    int scene_change = 1;
    int score = 0;
    char scorestr[10];
    int fdmem = open("/dev/mem",O_RDWR);
    if (fdmem < 0){ printf("Error opening /dev/mem"); return -1;}

    void* gpio_ctr = mmap(0, 4096, PROT_READ + PROT_WRITE, MAP_SHARED, fdmem, GPIO_BASE);
    if(gpio_ctr == MAP_FAILED) {printf("mmap error"); return -1;}
    
    set_gpio_input(gpio_ctr,4);
    set_gpio_pullup(gpio_ctr,4);
    set_gpio_input(gpio_ctr,27);
    set_gpio_pullup(gpio_ctr,27);
    set_gpio_input(gpio_ctr,12);
    set_gpio_pullup(gpio_ctr,12);

    int gpio_4_value; // left switch
    int gpio_27_value; // right switch
    int gpio_12_value; // fire switch
    int fire_switch_stat = 0;

    int i2c_fd = open("/dev/i2c-1",O_RDWR);
    if(i2c_fd < 0){
        printf("err opening device\n");
        return -1;
    }

    if(ioctl(i2c_fd, I2C_SLAVE, SSD1306_I2C_DEV) < 0){
        printf("err setting i2c slave address\n");
        return -1;
    }

    ssd1306_Init(i2c_fd);

    //erasing
    uint8_t* data = (uint8_t*)malloc(S_WIDTH*S_PAGES);
    for(int x = 0; x < S_WIDTH; x++){
        for(int y = 0; y < S_PAGES; y++){
            data[S_WIDTH*y + x] = 0x00;
        }
    }
    update_full(i2c_fd,data);
    free(data);

    player.x = 64;
    player.y = 7;
    uint8_t* shipdata = (uint8_t*)malloc(8*1);
    for(int i = 0; i < 8; i++){
        shipdata[i] = ship[i];
    }

    write_str(i2c_fd, "SPACE EMBEDDERS", 20, 1);
    write_str(i2c_fd, "Press fire key",23,6);
    write_str(i2c_fd, "to start",39,7);

    while(1){
        get_gpio_input_value(gpio_ctr,4,&gpio_4_value);
        get_gpio_input_value(gpio_ctr,27,&gpio_27_value);
        get_gpio_input_value(gpio_ctr,12,&gpio_12_value);
        if(current_scene == 1 && !gpio_12_value){
            fire_switch_stat = 1;
            current_scene = 2;
            uint8_t* data = (uint8_t*)malloc(S_WIDTH*S_PAGES);
            for(int x = 0; x < S_WIDTH; x++){
                for(int y = 0; y < S_PAGES; y++){
                    data[S_WIDTH*y + x] = 0x00;
                }
            }
            update_full(i2c_fd,data);
            free(data);
            update_area(i2c_fd,shipdata,player.x,player.y,8,1);
        }
        if (current_scene == 2){
            sprintf(scorestr,"%d",score);
            write_str(i2c_fd, "SCORE ", 27, 0);
            write_str(i2c_fd, scorestr, 64, 0);
            if(!(!gpio_4_value && !gpio_27_value)){
                if(!gpio_4_value){
                    player.x--;
                    update_area(i2c_fd,shipdata,player.x,player.y,8,1);
                }
                if(!gpio_27_value){
                    player.x++;
                    update_area(i2c_fd,shipdata,player.x,player.y,8,1);
                }
            }
            if(!gpio_12_value && fire_switch_stat == 0){
                fire_switch_stat = 1;
            }
        }
        if(gpio_12_value && fire_switch_stat == 1) fire_switch_stat = 0;
    }
    free(shipdata);
    close(i2c_fd);
    return 0;
}