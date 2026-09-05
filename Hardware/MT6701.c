#include "stm32f10x.h"
#include "Delay.h"
#include "MT6701.h"

/**
 * 时序模式选择：
 * 1：按照MT6701数据手册，CLK空闲为高，下降沿采样，即SPI模式2
 * 0：保持原HAL工程设置，CLK空闲为低，下降沿采样，即SPI模式1
 */
#define MT6701_USE_DATASHEET_TIMING	1

/*全局变量*********************/

/**
 * MT6701 DMA接收数组
 * DMA会在后台修改该数组，因此需要使用volatile修饰
 */
volatile uint8_t MT6701_RxData[MT6701_FRAME_SIZE];


/**
 * SPI发送占位数据
 * MT6701的SSI接口只输出数据，不接收MOSI数据
 * STM32仍需发送占位数据来产生24个时钟
 */
static uint8_t MT6701_TxDummy = 0xFF;

/*通信状态标志*/
static volatile uint8_t MT6701_Busy;
static volatile uint8_t MT6701_DataReady;
static volatile uint8_t MT6701_DMAError;

/*********************全局变量*/


/*引脚配置*********************/

/**
 * 函    数：MT6701写片选信号
 * 参    数：BitValue 要写入CSN的电平，范围：0/1
 * 返 回 值：无
 * 说    明：PA4连接MT6701的CSN，低电平开始一帧传输
 */
static void MT6701_W_CS(uint8_t BitValue)
{
    GPIO_WriteBit(GPIOA, GPIO_Pin_4, (BitAction)BitValue);
}

/**
 * 函    数：MT6701 GPIO初始化
 * 参    数：无
 * 返 回 值：无
 * 说    明：PA4=CSN，PA5=CLK，PA6=DO，PA7=MOSI占位输出
 */
static void MT6701_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    /*开启GPIOA时钟*/
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    /*在切换为输出模式前先设置CSN和CLK的空闲电平*/
    GPIO_SetBits(GPIOA, GPIO_Pin_4 | GPIO_Pin_5);
    
    /*配置PA4为推挽输出，用于软件控制CSN*/
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /*配置PA5和PA7为复用推挽输出，分别作为SCK和MOSI*/
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7;

    /*配置PA6为浮空输入，接收MT6701的DO数据*/
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /*通信空闲时CSN保持高电平*/
    MT6701_w_cs(1);
}

/*********************引脚配置*/


/*SPI配置*********************/

/**
 * 函    数：MT6701 SPI1初始化
 * 参    数：无
 * 返 回 值：无
 * 说    明：SPI1主机、8位、MSB先行、软件片选、PCLK2/64
 */
static void MT6701_SPI_Init(void)
{
	SPI_InitTypeDef SPI_InitStructure;

	/*开启SPI1时钟*/
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);

	/*恢复SPI1默认配置*/
	SPI_I2S_DeInit(SPI1);

	/*配置SPI1工作参数*/
	SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
	SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
	SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;

#if MT6701_USE_DATASHEET_TIMING
	/*模式2：CLK空闲为高，在第一个边沿（下降沿）采样*/
	SPI_InitStructure.SPI_CPOL = SPI_CPOL_High;
	SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
#else
	/*模式1：保持原HAL工程的CLK空闲电平设置*/
	SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
	SPI_InitStructure.SPI_CPHA = SPI_CPHA_2Edge;
#endif

	SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
	SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_64;
	SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
	SPI_InitStructure.SPI_CRCPolynomial = 7;
	SPI_Init(SPI1, &SPI_InitStructure);

	/*软件NSS模式下将内部NSS置高，防止主机模式错误*/
	SPI_NSSInternalSoftwareConfig(SPI1, SPI_NSSInternalSoft_Set);

	/*使能SPI1*/
	SPI_Cmd(SPI1, ENABLE);
}