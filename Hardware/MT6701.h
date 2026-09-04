#ifndef __MT6701_H
#define __MT6701_H

 #include <stdint.h>
/*
*MT6701接线 Default wirting:
*PA4 -> MT6701 CSN
*PA5 -> MT6701 CLK  (SPI1_SCK)
*PA6 -> MT6701 DO   (SPI1_MISO)
*PA7 -> MT6701 MOSI (SPI1_MOSI)
*/

//定义一帧数据长度3*8=24bit,
//24位数据分为，前14位角度数据，中四位状态位，后6位CRC校验码
#define MT6701_FRAME_SIZE 3U

//编码分辨率，MT6701输出角度数据是14位，2^14=16384
//一圈的角度数据范围是0~16383，对应0~360度
//所以一位角度数据对应的角度是360/16384=0.02197265625度
#define MT6701_ANGLE_COUNTS 16384U

//SPI时序选择
#ifndef MT6701_USE_DATASHEET_TIMING
#define MT6701_USE_DATASHEET_TIMING 1
#endif

//
typedef enum
{
    MT6701_READ_NONE = 0,
    MT6701_READ_OK,
    MT6701_READ_CRC_ERROR
} MT6701_ReadResult_t;

//
typedef struct
{
    uint32_t raw24;          //原始24位数据
    uint16_t angle_raw;      //原始角度数据，14位
    uint8_t status;          //状态位，4位
    uint8_t crc_received;    //接受到的CRC校验码，6位
    uint8_t crc_calculated;  //计算得到的CRC校验码，6位
    float angle_degree;      //角度数据，单位：度
} MT6701_Frame_t;

//
extern volatile uint8_t mt6701_rx_data[MT6701_FRAME_SIZE];

void MT6701_SPI_Init(void);
void MT6701_SPI_DeInit(void);

//启动一个DMA读取
uint8_t MT6701_SPI_Start(void);

//查询DMA忙状态
uint8_t MT6701_SPI_IsBusy(void);

//查询DMA硬件错误
uint8_t MT6701_SPI_HasError(void);

//获取并解析一帧数据
MT6701_ReadResult_t MT6701_SPI_GetFrame(MT6701_Frame_t *Frame);

//CRC计算函数
uint8_t MT6701_CalculateCRC6(uint32_t payload18);

//DMA中断服务函数
void MT6701_SPI_DMA_RX_IRQHandler(void);
void MT6701_SPI_DMA_TX_IRQHandler(void);

 #endif
