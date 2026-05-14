#include <reg51.h>

#define u8 unsigned char
#define u16 unsigned int
#define u32 unsigned long

/* ===================== 引脚定义 ===================== */
/* 数码管锁存控制（沿用类似已有项目的单线锁存方式） */
sbit SEG_LATCH = P2^7;

/* 蜂鸣器 */
sbit BUZZER = P3^2;

/* DS1302 */
sbit DS_RST  = P3^4;
sbit DS_IO   = P3^5;
sbit DS_SCLK = P3^7;

/* AT24C04 (I2C) */
sbit I2C_SCL = P3^0;
sbit I2C_SDA = P3^1;

/* LCD1602 4位模式 */
sbit LCD_RS = P2^0;
sbit LCD_RW = P2^1;
sbit LCD_EN = P2^2;
sbit LCD_D4 = P2^3;
sbit LCD_D5 = P2^4;
sbit LCD_D6 = P2^5;
sbit LCD_D7 = P2^6;

/* 4x4 矩阵键盘（布局：7~9 / 4~6 / 1~3 / 0） */
sbit R1 = P1^0;
sbit R2 = P1^1;
sbit R3 = P1^2;
sbit R4 = P1^3;
sbit C1 = P1^4;
sbit C2 = P1^5;
sbit C3 = P1^6;
sbit C4 = P1^7;

/* ===================== 常量定义 ===================== */
#define KEY_NONE   0xFF
#define KEY_COLON  0xF0
#define KEY_BACK   0xF1
#define KEY_ENTER  0xF2
#define KEY_MODE   0xF3
#define KEY_VIEW   0xF4
#define KEY_TOGGLE 0xF5

#define SCREEN_TIME 0
#define SCREEN_DATE 1
#define SCREEN_WEEK 2

#define ALARM_COUNT 2

#define EE_SIGN_ADDR  0x00
#define EE_DATA_BASE  0x01
#define EE_SIGN_VALUE 0x5A

/* ===================== 全局变量 ===================== */
volatile u8 rtc_year = 24, rtc_month = 1, rtc_date = 1;
volatile u8 rtc_hour = 12, rtc_min = 0, rtc_sec = 0, rtc_week = 1;

volatile u8 disp_idx = 0;
volatile u8 disp_buf[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
volatile u8 flash_flag = 0;
volatile u8 current_screen = SCREEN_TIME;
volatile bit sec_flag = 0;

volatile u16 tick_2ms = 0;
volatile u16 screen_tick = 0;
volatile u16 second_tick = 0;
volatile u16 beep_ticks = 0;
volatile u8  beep_div = 0;

bit chime_enable = 1;
bit alarm_master_enable = 1;

typedef struct
{
    u8 hour;
    u8 min;
    bit enable;
    u16 last_stamp;
} Alarm_t;

Alarm_t alarms[ALARM_COUNT];

u8 edit_buf[3];
u8 edit_len;

/* 共阳数码管段码（0~9、-、空） */
code u8 SEG_CODE[12] = {
    0xC0,0xF9,0xA4,0xB0,0x99,
    0x92,0x82,0xF8,0x80,0x90,
    0xBF,0xFF
};

/* 位选码 */
code u8 DIG_CODE[8] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80};

/* ===================== 基础延时 ===================== */
void DelayTicks(u16 t)
{
    while(t--) ;
}

void DelayMs(u16 ms)
{
    u16 i;
    while(ms--)
    {
        /* 约按 12MHz 晶振估算的空转延时 */
        for(i = 0; i < 120; i++);
    }
}

/* ===================== LCD1602 ===================== */
void LcdPulse(void)
{
    LCD_EN = 1;
    DelayTicks(30);
    LCD_EN = 0;
}

void LcdWrite4(u8 nib)
{
    LCD_D4 = (bit)(nib & 0x01);
    LCD_D5 = (bit)(nib & 0x02);
    LCD_D6 = (bit)(nib & 0x04);
    LCD_D7 = (bit)(nib & 0x08);
    LcdPulse();
}

void LcdWriteByte(u8 rs, u8 dat)
{
    LCD_RS = rs;
    LCD_RW = 0;
    LcdWrite4(dat >> 4);
    LcdWrite4(dat & 0x0F);
    DelayMs(2);
}

void LcdWriteCmd(u8 cmd)
{
    LcdWriteByte(0, cmd);
}

void LcdWriteData(u8 dat)
{
    LcdWriteByte(1, dat);
}

void LcdSetPos(u8 col, u8 row)
{
    if(row == 0) LcdWriteCmd(0x80 + col);
    else         LcdWriteCmd(0xC0 + col);
}

void LcdPrint(char *s)
{
    while(*s)
    {
        LcdWriteData(*s);
        s++;
    }
}

void LcdPrint2(u8 v)
{
    LcdWriteData((v / 10) + '0');
    LcdWriteData((v % 10) + '0');
}

void LcdClear(void)
{
    LcdWriteCmd(0x01);
    DelayMs(3);
}

void LcdInit(void)
{
    LCD_RS = 0;
    LCD_RW = 0;
    LCD_EN = 0;

    DelayMs(20);
    LcdWrite4(0x03); DelayMs(5);
    LcdWrite4(0x03); DelayMs(2);
    LcdWrite4(0x03); DelayMs(2);
    LcdWrite4(0x02); DelayMs(2);

    LcdWriteCmd(0x28); /* 4bit, 2line */
    LcdWriteCmd(0x0C); /* display on */
    LcdWriteCmd(0x06); /* entry mode */
    LcdClear();
}

void LcdShowStatus(void)
{
    LcdClear();
    LcdSetPos(0,0);
    LcdPrint("CLK ");
    LcdPrint2(rtc_hour); LcdWriteData(':'); LcdPrint2(rtc_min); LcdWriteData(':'); LcdPrint2(rtc_sec);
    LcdSetPos(0,1);
    LcdPrint("CH:"); LcdWriteData(chime_enable ? '1' : '0');
    LcdPrint(" ALM:"); LcdWriteData(alarm_master_enable ? '1' : '0');
}

/* ===================== DS1302 ===================== */
u8 DecToBcd(u8 d) { return ((d / 10) << 4) | (d % 10); }
u8 BcdToDec(u8 b) { return ((b >> 4) * 10) + (b & 0x0F); }

void DsStart(void)
{
    DS_RST = 0;
    DS_SCLK = 0;
    DS_RST = 1;
}

void DsWriteByte(u8 dat)
{
    u8 i;
    for(i = 0; i < 8; i++)
    {
        DS_IO = (bit)(dat & 0x01);
        DS_SCLK = 0;
        DS_SCLK = 1;
        dat >>= 1;
    }
}

u8 DsReadByte(void)
{
    u8 i, dat = 0;
    for(i = 0; i < 8; i++)
    {
        dat >>= 1;
        DS_SCLK = 1;
        DS_SCLK = 0;
        if(DS_IO) dat |= 0x80;
    }
    return dat;
}

void DsWriteReg(u8 addr, u8 dat)
{
    DsStart();
    DsWriteByte(addr);
    DsWriteByte(dat);
    DS_SCLK = 1;
    DS_RST = 0;
}

u8 DsReadReg(u8 addr)
{
    u8 d;
    DsStart();
    DsWriteByte(addr);
    d = DsReadByte();
    DS_SCLK = 1;
    DS_RST = 0;
    return d;
}

void DsReadTime(void)
{
    rtc_sec   = BcdToDec(DsReadReg(0x81) & 0x7F);
    rtc_min   = BcdToDec(DsReadReg(0x83));
    rtc_hour  = BcdToDec(DsReadReg(0x85) & 0x3F);
    rtc_date  = BcdToDec(DsReadReg(0x87));
    rtc_month = BcdToDec(DsReadReg(0x89));
    rtc_week  = BcdToDec(DsReadReg(0x8B));
    rtc_year  = BcdToDec(DsReadReg(0x8D));
}

void DsWriteTime(u8 yy, u8 mm, u8 dd, u8 hh, u8 mi, u8 ss, u8 ww)
{
    DsWriteReg(0x8E, 0x00);
    DsWriteReg(0x80, DecToBcd(ss));
    DsWriteReg(0x82, DecToBcd(mi));
    DsWriteReg(0x84, DecToBcd(hh));
    DsWriteReg(0x86, DecToBcd(dd));
    DsWriteReg(0x88, DecToBcd(mm));
    DsWriteReg(0x8A, DecToBcd(ww));
    DsWriteReg(0x8C, DecToBcd(yy));
    DsWriteReg(0x8E, 0x80);
}

/* ===================== AT24C04 ===================== */
void I2cDelay(void)
{
    DelayTicks(10);
}

void I2cStart(void)
{
    I2C_SDA = 1; I2C_SCL = 1; I2cDelay();
    I2C_SDA = 0; I2cDelay();
    I2C_SCL = 0;
}

void I2cStop(void)
{
    I2C_SDA = 0; I2C_SCL = 1; I2cDelay();
    I2C_SDA = 1; I2cDelay();
}

bit I2cAckOk(void)
{
    u16 t = 500;
    I2C_SDA = 1;
    I2C_SCL = 1;
    while(I2C_SDA && t--) ;
    I2C_SCL = 0;
    return (t != 0);
}

void I2cWriteByte(u8 dat)
{
    u8 i;
    for(i = 0; i < 8; i++)
    {
        I2C_SDA = (bit)(dat & 0x80);
        dat <<= 1;
        I2C_SCL = 1;
        I2cDelay();
        I2C_SCL = 0;
        I2cDelay();
    }
    I2C_SDA = 1;
}

u8 I2cReadByte(bit ack)
{
    u8 i, dat = 0;
    I2C_SDA = 1;
    for(i = 0; i < 8; i++)
    {
        I2C_SCL = 1;
        dat <<= 1;
        if(I2C_SDA) dat |= 0x01;
        I2cDelay();
        I2C_SCL = 0;
        I2cDelay();
    }
    I2C_SDA = ack ? 0 : 1;
    I2C_SCL = 1; I2cDelay();
    I2C_SCL = 0; I2cDelay();
    I2C_SDA = 1;
    return dat;
}

void EepromWriteByte(u8 addr, u8 dat)
{
    I2cStart();
    I2cWriteByte(0xA0);
    I2cAckOk();
    I2cWriteByte(addr);
    I2cAckOk();
    I2cWriteByte(dat);
    I2cAckOk();
    I2cStop();
    DelayMs(10);
}

u8 EepromReadByte(u8 addr)
{
    u8 dat;
    I2cStart();
    I2cWriteByte(0xA0);
    I2cAckOk();
    I2cWriteByte(addr);
    I2cAckOk();

    I2cStart();
    I2cWriteByte(0xA1);
    I2cAckOk();
    dat = I2cReadByte(0);
    I2cStop();
    return dat;
}

void SaveConfig(void)
{
    EepromWriteByte(EE_SIGN_ADDR, EE_SIGN_VALUE);
    EepromWriteByte(EE_DATA_BASE + 0, chime_enable ? 1 : 0);
    EepromWriteByte(EE_DATA_BASE + 1, alarm_master_enable ? 1 : 0);
    EepromWriteByte(EE_DATA_BASE + 2, alarms[0].hour);
    EepromWriteByte(EE_DATA_BASE + 3, alarms[0].min);
    EepromWriteByte(EE_DATA_BASE + 4, alarms[0].enable ? 1 : 0);
    EepromWriteByte(EE_DATA_BASE + 5, alarms[1].hour);
    EepromWriteByte(EE_DATA_BASE + 6, alarms[1].min);
    EepromWriteByte(EE_DATA_BASE + 7, alarms[1].enable ? 1 : 0);
}

void LoadConfig(void)
{
    if(EepromReadByte(EE_SIGN_ADDR) != EE_SIGN_VALUE)
    {
        chime_enable = 1;
        alarm_master_enable = 1;
        alarms[0].hour = 7; alarms[0].min = 0; alarms[0].enable = 1;
        alarms[1].hour = 12; alarms[1].min = 0; alarms[1].enable = 0;
        SaveConfig();
        return;
    }

    chime_enable = EepromReadByte(EE_DATA_BASE + 0) ? 1 : 0;
    alarm_master_enable = EepromReadByte(EE_DATA_BASE + 1) ? 1 : 0;

    alarms[0].hour = EepromReadByte(EE_DATA_BASE + 2);
    alarms[0].min  = EepromReadByte(EE_DATA_BASE + 3);
    alarms[0].enable = EepromReadByte(EE_DATA_BASE + 4) ? 1 : 0;

    alarms[1].hour = EepromReadByte(EE_DATA_BASE + 5);
    alarms[1].min  = EepromReadByte(EE_DATA_BASE + 6);
    alarms[1].enable = EepromReadByte(EE_DATA_BASE + 7) ? 1 : 0;

    if(alarms[0].hour > 23) alarms[0].hour = 7;
    if(alarms[1].hour > 23) alarms[1].hour = 12;
    if(alarms[0].min > 59) alarms[0].min = 0;
    if(alarms[1].min > 59) alarms[1].min = 0;
}

/* ===================== 数码管显示 ===================== */
void SegSend(u8 idx, u8 seg)
{
    SEG_LATCH = 0;
    SEG_LATCH = 1;
    P0 = DIG_CODE[idx];
    SEG_LATCH = 0;
    P0 = seg;
}

u8 DigitToSeg(u8 d)
{
    if(d <= 9) return SEG_CODE[d];
    return SEG_CODE[11];
}

void BuildDisplayBuffer(void)
{
    if(current_screen == SCREEN_TIME)
    {
        disp_buf[0] = DigitToSeg(rtc_sec % 10);
        disp_buf[1] = DigitToSeg(rtc_sec / 10);
        disp_buf[2] = SEG_CODE[10];
        disp_buf[3] = DigitToSeg(rtc_min % 10);
        disp_buf[4] = DigitToSeg(rtc_min / 10);
        disp_buf[5] = SEG_CODE[10];
        disp_buf[6] = DigitToSeg(rtc_hour % 10);
        disp_buf[7] = DigitToSeg(rtc_hour / 10);
    }
    else if(current_screen == SCREEN_DATE)
    {
        disp_buf[0] = DigitToSeg(rtc_date % 10);
        disp_buf[1] = DigitToSeg(rtc_date / 10);
        disp_buf[2] = DigitToSeg(rtc_month % 10);
        disp_buf[3] = DigitToSeg(rtc_month / 10);
        disp_buf[4] = DigitToSeg(rtc_year % 10);
        disp_buf[5] = DigitToSeg(rtc_year / 10);
        disp_buf[6] = DigitToSeg(0);
        disp_buf[7] = DigitToSeg(2);
    }
    else
    {
        disp_buf[0] = DigitToSeg(rtc_week % 10);
        disp_buf[1] = SEG_CODE[10];
        disp_buf[2] = SEG_CODE[10];
        disp_buf[3] = SEG_CODE[10];
        disp_buf[4] = SEG_CODE[10];
        disp_buf[5] = SEG_CODE[10];
        disp_buf[6] = SEG_CODE[10];
        disp_buf[7] = SEG_CODE[10];
    }
}

/* ===================== 按键 ===================== */
u8 KeyScan(void)
{
    u8 i, j;
    u8 key;

    u8 Key_Map[4][4] = {
        {7, 8, 9, KEY_COLON},
        {4, 5, 6, KEY_BACK},
        {1, 2, 3, KEY_ENTER},
        {KEY_MODE, 0, KEY_VIEW, KEY_TOGGLE}
    };

    for(i = 0; i < 4; i++)
    {
        R1 = R2 = R3 = R4 = 1;
        if(i == 0) R1 = 0;
        if(i == 1) R2 = 0;
        if(i == 2) R3 = 0;
        if(i == 3) R4 = 0;

        for(j = 0; j < 4; j++)
        {
            bit pressed = 0;
            if(j == 0 && C1 == 0) pressed = 1;
            if(j == 1 && C2 == 0) pressed = 1;
            if(j == 2 && C3 == 0) pressed = 1;
            if(j == 3 && C4 == 0) pressed = 1;

            if(pressed)
            {
                DelayMs(20);
                key = Key_Map[i][j];
                while((j == 0 && C1 == 0) || (j == 1 && C2 == 0) || (j == 2 && C3 == 0) || (j == 3 && C4 == 0));
                return key;
            }
        }
    }

    return KEY_NONE;
}

/* ===================== 业务逻辑 ===================== */
void StartBeepMs(u16 ms)
{
    u16 t = ms / 2;
    if(t > beep_ticks) beep_ticks = t;
}

u16 BuildMinuteStamp(void)
{
    return (u16)rtc_date * 1440 + (u16)rtc_hour * 60 + rtc_min;
}

void CheckAlarmAndChime(void)
{
    u8 i;
    u16 stamp;

    if(rtc_sec != 0) return;

    if(chime_enable && rtc_min == 0)
    {
        StartBeepMs(1000);
    }

    if(!alarm_master_enable) return;

    stamp = BuildMinuteStamp();
    for(i = 0; i < ALARM_COUNT; i++)
    {
        if(alarms[i].enable && rtc_hour == alarms[i].hour && rtc_min == alarms[i].min)
        {
            if(alarms[i].last_stamp != stamp)
            {
                alarms[i].last_stamp = stamp;
                StartBeepMs(4000);
            }
        }
    }
}

bit InputNumberField(char *title, u8 digits, u8 minv, u8 maxv, u8 *out)
{
    u8 idx = 0;
    u8 key;
    u8 value;
    u8 k;

    edit_len = digits;
    edit_buf[0] = edit_buf[1] = edit_buf[2] = ' ';

    LcdClear();
    LcdSetPos(0,0);
    LcdPrint(title);
    LcdSetPos(0,1);
    LcdPrint("VAL:");

    while(1)
    {
        key = KeyScan();
        if(key == KEY_NONE) continue;

        if(key <= 9)
        {
            if(idx < digits)
            {
                edit_buf[idx] = key + '0';
                LcdSetPos(4 + idx, 1);
                LcdWriteData(edit_buf[idx]);
                idx++;
            }
        }
        else if(key == KEY_BACK)
        {
            if(idx > 0)
            {
                idx--;
                edit_buf[idx] = ' ';
                LcdSetPos(4 + idx, 1);
                LcdWriteData(' ');
            }
        }
        else if(key == KEY_MODE)
        {
            return 0;
        }
        else if(key == KEY_ENTER)
        {
            if(idx == digits)
            {
                if(digits == 1)
                {
                    value = edit_buf[0] - '0';
                }
                else if(digits == 2)
                {
                    value = (edit_buf[0] - '0') * 10 + (edit_buf[1] - '0');
                }
                else if(digits == 3)
                {
                    value = 0;
                    for(k = 0; k < 3; k++) value = value * 10 + (edit_buf[k] - '0');
                }
                else
                {
                    value = 0;
                    for(k = 0; k < digits && k < 3; k++) value = value * 10 + (edit_buf[k] - '0');
                }
                if(value >= minv && value <= maxv)
                {
                    *out = value;
                    return 1;
                }
                else
                {
                    LcdSetPos(8,1);
                    LcdPrint("ERR");
                }
            }
        }
    }
}

void EnterTimeSetting(void)
{
    u8 yy = rtc_year, mm = rtc_month, dd = rtc_date;
    u8 hh = rtc_hour, mi = rtc_min, ss = rtc_sec, ww = rtc_week;

    if(!InputNumberField("SET YEAR(00-99)", 2, 0, 99, &yy)) return;
    if(!InputNumberField("SET MONTH(1-12)",2, 1, 12, &mm)) return;
    if(!InputNumberField("SET DAY(1-31)",  2, 1, 31, &dd)) return;
    if(!InputNumberField("SET HOUR(0-23)", 2, 0, 23, &hh)) return;
    if(!InputNumberField("SET MIN(0-59)",  2, 0, 59, &mi)) return;
    if(!InputNumberField("SET SEC(0-59)",  2, 0, 59, &ss)) return;
    if(!InputNumberField("SET WEEK(1-7)",  1, 1,  7, &ww)) return;

    DsWriteTime(yy, mm, dd, hh, mi, ss, ww);
    DsReadTime();

    LcdClear();
    LcdSetPos(0,0);
    LcdPrint("TIME UPDATED");
    DelayMs(600);
    LcdShowStatus();
}

void EnterAlarmSetting(void)
{
    u8 h, m, en;

    h = alarms[0].hour;
    m = alarms[0].min;
    if(InputNumberField("AL1 HOUR(0-23)", 2, 0, 23, &h)) alarms[0].hour = h;
    if(InputNumberField("AL1 MIN(0-59)",  2, 0, 59, &m)) alarms[0].min = m;
    en = alarms[0].enable ? 1 : 0;
    if(InputNumberField("AL1 EN(0/1)",    1, 0,  1, &en)) alarms[0].enable = en ? 1 : 0;

    h = alarms[1].hour;
    m = alarms[1].min;
    if(InputNumberField("AL2 HOUR(0-23)", 2, 0, 23, &h)) alarms[1].hour = h;
    if(InputNumberField("AL2 MIN(0-59)",  2, 0, 59, &m)) alarms[1].min = m;
    en = alarms[1].enable ? 1 : 0;
    if(InputNumberField("AL2 EN(0/1)",    1, 0,  1, &en)) alarms[1].enable = en ? 1 : 0;

    SaveConfig();

    LcdClear();
    LcdSetPos(0,0);
    LcdPrint("ALARM SAVED");
    DelayMs(600);
    LcdShowStatus();
}

void ShowAlarmParams(void)
{
    u8 key;
    while(1)
    {
        LcdClear();
        LcdSetPos(0,0);
        LcdPrint("A1 ");
        LcdPrint2(alarms[0].hour); LcdWriteData(':'); LcdPrint2(alarms[0].min);
        LcdPrint(alarms[0].enable ? " ON " : "OFF ");

        LcdSetPos(0,1);
        LcdPrint("A2 ");
        LcdPrint2(alarms[1].hour); LcdWriteData(':'); LcdPrint2(alarms[1].min);
        LcdPrint(alarms[1].enable ? " ON " : "OFF ");

        key = KeyScan();
        if(key == KEY_VIEW || key == KEY_MODE)
        {
            LcdShowStatus();
            return;
        }
        else if(key == KEY_ENTER)
        {
            EnterAlarmSetting();
            return;
        }
        else if(key == KEY_TOGGLE)
        {
            alarm_master_enable = !alarm_master_enable;
            SaveConfig();
        }
        else if(key == KEY_COLON)
        {
            chime_enable = !chime_enable;
            SaveConfig();
        }
    }
}

void HandleNormalKey(u8 key)
{
    if(key == KEY_MODE)
    {
        EnterTimeSetting();
    }
    else if(key == KEY_VIEW)
    {
        ShowAlarmParams();
    }
    else if(key == KEY_TOGGLE)
    {
        alarm_master_enable = !alarm_master_enable;
        SaveConfig();
        LcdShowStatus();
    }
    else if(key == KEY_COLON)
    {
        chime_enable = !chime_enable;
        SaveConfig();
        LcdShowStatus();
    }
}

/* ===================== 定时器与中断 ===================== */
void Timer0Init(void)
{
    TMOD = 0x01;
    TH0 = (65536 - 2000) / 256;
    TL0 = (65536 - 2000) % 256;
    ET0 = 1;
    EA  = 1;
    TR0 = 1;
}

void Timer0Isr(void) interrupt 1
{
    TH0 = (65536 - 2000) / 256;
    TL0 = (65536 - 2000) % 256;

    tick_2ms++;
    screen_tick++;
    second_tick++;

    if(tick_2ms >= 250)
    {
        tick_2ms = 0;
        flash_flag = !flash_flag;
    }

    if(screen_tick >= 3000)
    {
        screen_tick = 0;
        current_screen++;
        if(current_screen > SCREEN_WEEK) current_screen = SCREEN_TIME;
    }

    if(second_tick >= 500)
    {
        second_tick = 0;
        sec_flag = 1;
    }

    if(beep_ticks > 0)
    {
        beep_ticks--;
        beep_div++;
        if(beep_div >= 10)
        {
            beep_div = 0;
            BUZZER = !BUZZER;
        }
    }
    else
    {
        BUZZER = 0;
        beep_div = 0;
    }

    SegSend(disp_idx, disp_buf[disp_idx]);
    disp_idx++;
    if(disp_idx >= 8) disp_idx = 0;
}

/* ===================== 主程序 ===================== */
void main(void)
{
    u8 key;
    u8 i;

    for(i = 0; i < ALARM_COUNT; i++) alarms[i].last_stamp = 0xFFFF;

    BUZZER = 0;
    I2C_SDA = 1;
    I2C_SCL = 1;

    LcdInit();
    LoadConfig();

    DsWriteReg(0x8E, 0x00);
    DsWriteReg(0x80, 0x00);
    DsWriteReg(0x8E, 0x80);

    DsReadTime();
    LcdShowStatus();
    BuildDisplayBuffer();
    Timer0Init();

    while(1)
    {
        key = KeyScan();
        if(key != KEY_NONE) HandleNormalKey(key);

        if(sec_flag)
        {
            sec_flag = 0;
            DsReadTime();
            CheckAlarmAndChime();
            BuildDisplayBuffer();
            LcdShowStatus();
        }
    }
}
