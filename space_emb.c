#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include "fonts/renew_font.h"
#include "ship.h"
#include "enemy1.h"
#include "enemy2.h"
#include "bullet.h"

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
#define enemy_WIDTH 8
#define enemy_HEIGHT 8
#define enemy_MOVE 2
#define NUM_FRAMES (S_WIDTH/en)


struct enemies{
	int x;
	int y;
	int alive; // 0 = dead, 1 = alive
	uint8_t* data;
};

struct ship{
	int x;
	int y;
};

struct Missile{
	int x;
	int y;
	int alive;  // 0 dead, 1 alive
};

struct pagepos{
	int page;
	int y;
};

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


/*
 * @Name: pos_converter
 * @Param: original pos
 * @Return: converted position - page&byteposition (struct)
 * @Description: original pos converts into page&bytes position 
 * @Author: Keonyoung Shim
 */

struct pagepos pos_converter(int y){
	int page = y / 8;
	int pos = y % 8;
	struct pagepos pgp;
	pgp.page = page;
	pgp.y = pos;
	return pgp;
}



/*
 * @Name: update_area_missiles
 * @Param: i2c_fd, missiles pos
 * @Return: void
 * @Description: updates all missiles area 
 * @Author: Keonyoung Shim
 */

void update_area_missiles(int i2c_fd, struct Missile * missiles){
	uint8_t *blank_buf =(uint8_t*)malloc(2*1);
	blank_buf = 0x00;
	for(int i=0; i<100; i++){
		if(missiles[i].alive){  // missile valid 한 것만 처리
			struct pagepos pgp = pos_converter(missiles[i].y);  // page 위치 찾기
			if(pgp.y == 6){ // 아래에 걸칠 때
				uint8_t *part1_buf =(uint8_t*)malloc(2*1);
				uint8_t *part2_buf =(uint8_t*)malloc(2*1);

				part1_buf[0] = 0x80;
				part1_buf[1] = 0x80;
				part2_buf[0] = 0x01;
				part2_buf[1] = 0x01;

				update_area(i2c_fd, part1_buf, missiles[i].x, pgp.page, 2, 1);
				if(pgp.page<7){
					update_area(i2c_fd, part2_buf, missiles[i].x, pgp.page+1, 2, 1);
				}
				if(pgp.page<6){
					update_area(i2c_fd, blank_buf, missiles[i].x, pgp.page+2, 2, 1);
				}
				free(part1_buf);
				free(part2_buf);
			} else { // page 하나로 처리할 때
				uint8_t *buf =(uint8_t*)malloc(2*1);
				uint8_t base = 0x03;
				base >>= pgp.y;
				buf[0] = base;
				buf[1] = base;
				update_area(i2c_fd, buf, missiles[i].x, pgp.page, 2, 1);
				if(pgp.page<7){
					update_area(i2c_fd, blank_buf, missiles[i].x, pgp.page+1, 2, 1);
				}
				free(buf);
			}
		}
	}
	free(blank_buf);
}


/*
 * @Name: missile_launched
 * @Param: missiles array, missile index, launch position
 * @Return: updated missile_index (int)
 * @Description: adds a new missile from the player 
 * @Author: Keonyoung Shim
 */

int missile_launched(struct Missile * missiles, int missile_index, int x, int y){  // missile launching handler
	missiles[missile_index].x = x;
	missiles[missile_index].y = y;  // position assignment
	missiles[missile_index].alive = 1;  // set valid
	missile_index++;
	missile_index %= 100; // missile index arrangement (circular array)

	return missile_index;
}

/*
 * @Name: missiles_move
 * @Param: missile array, the amount of moves
 * @Return: void
 * @Description: moves all missiles valid by the designated amount of moves
 * @Author: Keonyoung Shim
 */

void missiles_move(struct Missile * missiles, int moves){  // missile moves here
	int i;
	for(i=0; i<100; i++){
		if(missiles[i].alive) // if the missile is valid one
			missiles[i].y -= moves;  // the missile goes up 
		if(missiles[i].y<=0)
			missiles[i].alive = 0;  // it dies when it goes out of bound
			continue;
	}
}


/*
 * @Name: isSamepos
 * @Param: enemy position, missile position
 * @Return: 0 met, 1 unmet (int)
 * @Description: get distance from the enemy to the missile and check these are met already. 
 * @Author: Keonyoung Shim
 */

int isSamepos(int ex, int ey, int mx, int my){
	int dist_x = ex - mx;
	int dist_y = ey*8 - my;
	if(dist_x < 0 || dist_y < 0)
		return 1;
	if(dist_x > 8 || dist_y > 8)
		return 1;
	return 0;
}


/*
 * @Name: isbombed
 * @Param: enemy position, missile position
 * @Return: the number of bombed enemy (int)
 * @Description: get distance from the enemy to the missile and check these have been crashed. 
 * @Author: Keonyoung Shim
 */

int isbombed(struct enemies * enemy, struct Missile * missiles, int i2c_fd, uint8_t * screencleardata){
	int enemies_len = 24;
	int missiles_len = 100;
	int ret_val = 0;
	int i, j;
	for(i = 0; i < enemies_len; i++){
		if(enemy[i].alive){
			for(j = 0; j < missiles_len; j++){
				if(missiles[j].alive){
					if(isSamepos(enemy[i].x, enemy[i].y, missiles[j].x, missiles[j].y) == 0){
						enemy[i].alive = 0;
						missiles[j].alive = 0;
						update_area(i2c_fd, screencleardata, enemy[i].x, enemy[i].y, 12, 1);
						ret_val++;
						break;
					}
				}
			}
		}
	}

	return ret_val;
}


int main() {

    int player_alive = 1;
    int current_scene = 1;
    int dir = 1; // enemy moving direction, 0 = left, 1 = right
    uint8_t* screencleardata;
    int fullscreencleared = 0; //used to check changing scene and clear full screen

    struct enemies enm[24];

    struct ship player;

	struct Missile missiles[100];

	int missile_index = 0;  // missile index

    //when missile hits enemy, make score + 1000 then
    //use sprintf to update score.
    //and you have to clear screen that is displaying score numbers
    //because past score will remain.
    //each x(garo; fucking hangul andoe) size of number is 6.
    //y is 8(1 page). so you need to allocate memory appropriately.
    int score = 0;  // 점수
    char scorestr[10];  // used to make score string
    sprintf(scorestr, "%d", score);

    int fdmem = open("/dev/mem",O_RDWR);
    if (fdmem < 0){ printf("Error opening /dev/mem"); return -1;}

    void* gpio_ctr = mmap(0, 4096, PROT_READ + PROT_WRITE, MAP_SHARED, fdmem, GPIO_BASE);
    if(gpio_ctr == MAP_FAILED) {printf("mmap error"); return -1;}
    
	/* gpio setup - buttons */
    set_gpio_input(gpio_ctr,4);
    set_gpio_pullup(gpio_ctr,4);
    set_gpio_input(gpio_ctr,27);
    set_gpio_pullup(gpio_ctr,27);
    set_gpio_input(gpio_ctr,12);
    set_gpio_pullup(gpio_ctr,12);

    int gpio_4_value; // left switch
    int gpio_27_value; // right switch
    int gpio_12_value; // fire switch
    int fire_switch_stat = 0; //fire switch status, used for bloking duplicate button input.

    int i2c_fd = open("/dev/i2c-1",O_RDWR);
    if(i2c_fd < 0){
        printf("err opening device\n");
        return -1;
    }

    if(ioctl(i2c_fd, I2C_SLAVE, SSD1306_I2C_DEV) < 0){
        printf("err setting i2c slave address\n");
        return -1;
    }

    ssd1306_Init(i2c_fd);  // i2c display initialization

    // initialization of buffer for display
    uint8_t* data = (uint8_t*)malloc(S_WIDTH*S_PAGES);
    for(int x = 0; x < S_WIDTH; x++){
        for(int y = 0; y < S_PAGES; y++){
            data[S_WIDTH*y + x] = 0x00;
        }
    }
    update_full(i2c_fd,data);  // display to black
    free(data); // ???

	// player의 현재 위치
    player.x = 59;
    player.y = 7;

    //first screen
    write_str(i2c_fd, "SPACE EMBEDDERS", 20, 1);
    write_str(i2c_fd, "Press fire key",23,6);
    write_str(i2c_fd, "to start",39,7);

    //data for display spaceship
    uint8_t* shipdata = (uint8_t*) malloc((ship_WIDTH+8)*ship_HEIGHT);
    for(int x = 0; x < 4; x++){ // for clearing
        shipdata[0+x] = 0x0;
        shipdata[ship_WIDTH+4+x] = 0x0;
    }
    for(int x = 0; x < ship_WIDTH; x++){
        shipdata[4+x] = ship[x];
    }

    //data for clearing 8x8 screen. used for enemy moving downward
    screencleardata = (uint8_t*) malloc(enemy_WIDTH*enemy_HEIGHT);
    for(int i = 0; i < 8; i++){
        screencleardata[i] = 0x0;
    }

    //data for display enemies
    for(int i = 0; i < 24; i++){
        enm[i].data = (uint8_t*) malloc((enemy_WIDTH+4)*enemy_HEIGHT);
        
        if(i%2){ // enemy type 2
            for(int j = 0; j < 8; j++){
                enm[i].data[j+2] = enemy2[j];
            }
        }
        else{ // enemy type 1
            for(int j = 0; j < 8; j++){
                enm[i].data[j+2] = enemy1[j];
            }
        }

        // for clearing
        enm[i].data[0] = 0x0;
        enm[i].data[1] = 0x0;
        enm[i].data[10] = 0x0;
        enm[i].data[11] = 0x0;

        // if not alive, dont calculate any more.
        // position
        enm[i].alive = 1;
        enm[i].x = (i%8)*12 + 18;
        enm[i].y = (23-i)/8 + 1;
    }

	// initialization of missiles
	for(int i=0; i<100; i++){
		missiles[i].alive = 0;
	}

    while(1){
        int downflag = 0; // used for enemy moving downward
		/* button 입력 받아오기 */
        get_gpio_input_value(gpio_ctr,4,&gpio_4_value);
        get_gpio_input_value(gpio_ctr,27,&gpio_27_value);
        get_gpio_input_value(gpio_ctr,12,&gpio_12_value);
        if(current_scene == 1 && !gpio_12_value){  // 처음 메뉴 화면 and fire button pressed
            fire_switch_stat = 1;  // missile 버튼 on
            current_scene = 2;  // 게임 화면 모드

			//clear screen to empty screen
            uint8_t* data = (uint8_t*)malloc(S_WIDTH*S_PAGES);
            for(int x = 0; x < S_WIDTH; x++){
                for(int y = 0; y < S_PAGES; y++){
                    data[S_WIDTH*y + x] = 0x00;
                }
            }
            update_full(i2c_fd,data);
            free(data);

            //display spaceship
            update_area(i2c_fd,shipdata,player.x,player.y,ship_WIDTH+8,1);
        }
        if (current_scene == 2){  // 실제 게임 화면
            write_str(i2c_fd, "Score ", 64-30, 0); // display score at upper screen
            write_str(i2c_fd, scorestr, 70, 0); //display score
            if(!(!gpio_4_value && !gpio_27_value)){  // 움직였다면
                if(!gpio_4_value){  // 좌로 움직였다면
                    if(player.x>=-4) player.x-=2;
                    update_area(i2c_fd,shipdata,player.x,player.y,ship_WIDTH+8,1);
                }
                if(!gpio_27_value){ // 우로 움직였다면
                    if(player.x <= 112)player.x+=2;
                    update_area(i2c_fd,shipdata,player.x,player.y,ship_WIDTH+8,1);
                }
            }
            if(!gpio_12_value && fire_switch_stat == 0){
                fire_switch_stat = 1;
                missile_launched(missiles, missile_index, player.x+5, player.y * 8);
            }

			missiles_move(missiles, 2);
			int gotscore = isbombed(enm, missiles, i2c_fd, screencleardata);
			score += gotscore;

			update_area_missiles(i2c_fd, missiles);
            //enemy position check to move down or side
            for(int i = 0; i < 24; i++){
                if(!enm[i].alive) continue;
                if(dir == 1 && enm[i].x >= 116){ // one of enemies is at right wall
                    downflag = 1;
                }
                else if(dir == 0 && enm[i].x <= 0){ // one of enemies is at left wall
                    downflag = 1;
                }
            }

            if(downflag){ // when one of enemies is at wall
                if(dir == 0) dir = 1; //make direction reverse
                else if(dir == 1) dir = 0; //make direction reverse
                for(int i = 0; i < 24; i++){
                    if(!enm[i].alive) continue; //if this enemy is dead, don't care
                    //first, clear screen of this enemy's position
                    update_area(i2c_fd,screencleardata,enm[i].x, enm[i].y,12,1);
                    //then, make y position + 1
                    enm[i].y++;
                    if(enm[i].y == 7) player_alive = 0; //player is dead
                }
                for(int i = 0; i < 24; i++){
                    if(!enm[i].alive) continue;
                    //display enemies at new position
                    update_area(i2c_fd,enm[i].data,enm[i].x, enm[i].y,12,1);
                }
                if(!player_alive){ // if enemies at bottom, display it then game over.
                    //to game over scene
                    current_scene = 3;
                    continue;
                }
            }
            else if(dir == 1){ //if enemies are moving to right
                for(int i = 0; i < 24; i++){
                    if(!enm[i].alive) continue;//if this enemy is dead, don't care
                    //x position + 1
                    enm[i].x++;
                    //display this enemy
                    update_area(i2c_fd,enm[i].data,enm[i].x, enm[i].y,12,1);
                }
            }
            else if(dir == 0){ //if enemies are moving to left
                for(int i = 0; i < 24; i++){
                    if(!enm[i].alive) continue;//if this enemy is dead, don't care
                    //x position -1
                    enm[i].x--;
                    //display this enemy
                    update_area(i2c_fd,enm[i].data,enm[i].x, enm[i].y,12,1);
                }
            }
        }
        if(current_scene == 3){ // game over scene
            if(!fullscreencleared){
                //first, when this time is first time to make game over scene,
                //it is necessary to make empty screen first.
                //but it doesn't have to when maintaining this scene.
                //because the screen will blink.
                //so, make empty screen when it is first time to make game over scene.
                fullscreencleared = 1; // and this is the indicator.
                uint8_t* data = (uint8_t*)malloc(S_WIDTH*S_PAGES);
                for(int x = 0; x < S_WIDTH; x++){
                    for(int y = 0; y < S_PAGES; y++){
                        data[S_WIDTH*y + x] = 0x00;
                    }
                }
                update_full(i2c_fd,data);  // display to black
                free(data); // ???
            }
            //game over phrases
            write_str(i2c_fd, "GAME  OVER",64-30, 1);
            write_str(i2c_fd, "YOUR SCORE", 64-30, 2);
            write_str(i2c_fd, scorestr, 64-15,3);
            write_str(i2c_fd, "PRESS FIRE", 64-30,5);
            write_str(i2c_fd, "To Play again", 64-39,6);

            //and when fire button is pressed
            if(!gpio_12_value && fire_switch_stat == 0){
                fullscreencleared = 0; // make this off
                fire_switch_stat = 1;  // missile 버튼 on
                current_scene = 2;  // 게임 화면 모드

			    //clear screen to empty screen
                uint8_t* data = (uint8_t*)malloc(S_WIDTH*S_PAGES);
                for(int x = 0; x < S_WIDTH; x++){
                    for(int y = 0; y < S_PAGES; y++){
                        data[S_WIDTH*y + x] = 0x00;
                    }
                }
                update_full(i2c_fd,data);
                free(data);

                //initialize position variables to first state
                player.x = 59;
                player.y = 7;
                player_alive = 1;
                for(int i = 0; i < 24; i++){
                    enm[i].alive = 1;
                    enm[i].x = (i%8)*12 + 18;
                    enm[i].y = (23-i)/8 + 1;
                }
                score = 0;
                sprintf(scorestr, "%d",score);

                update_area(i2c_fd,shipdata,player.x,player.y,ship_WIDTH+8,1);
                //that's all. it is the main game screen.
            }

        }
        //if fire button is not pressed, the button states restores to 0
        if(gpio_12_value && fire_switch_stat == 1) fire_switch_stat = 0;  // 미사일 못쏘는 상태
    }

    free(shipdata);
    close(i2c_fd);
    return 0;
}
