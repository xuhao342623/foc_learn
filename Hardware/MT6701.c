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
    MT6701_W_CS(1);
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

	/*恢复spi1默认配置*/
	SPI_I2S_DeInit(SPI1);

	/*配置SPI1参数*/
	SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
	SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
	SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
	SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
	SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_64;
	SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
	SPI_InitStructure.SPI_CRCPolynomial = 7;

#if MT6701_USE_DATASHEET_TIMING
	/*模式2：CLK空闲为高，在第一个边沿（下降沿）采样*/
	SPI_InitStructure.SPI_CPOL = SPI_CPOL_High;
	SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
#else
	/*模式1：保持原HAL工程的CLK空闲电平设置*/
	SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
	SPI_InitStructure.SPI_CPHA = SPI_CPHA_2Edge;
#endif

	SPI_Init(SPI1, &SPI_InitStructure);

	/*软件NSS模式下将内部NSS置高，防止主机模式错误*/
	SPI_NSSInternalSoftwareConfig(SPI1, SPI_NSSInternalSoft_Set);

	/*使能SPI1*/
	SPI_Cmd(SPI1, ENABLE);
}

/*********************SPI配置*/


/*DMA配置*********************/

/**
 * 函    数：MT6701 DMA初始化
 * 参    数：无
 * 返 回 值：无
 * 说    明：DMA1通道2用于SPI1_RX，DMA1通道3用于SPI1_TX
 */
static void MT6701_DMA_Init(void)
{
	DMA_InitTypeDef DMA_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;

	/*开启DMA1时钟*/
	RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

	//配置SPI1_RX：DMA1通道2
	DMA_DeInit(DMA1_Channel2);
	DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&(SPI1->DR);
	DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)&(MT6701_RxData);
	DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
	DMA_InitStructure.DMA_BufferSize = MT6701_FRAME_SIZE;
	DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
	DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
	DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
	DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
	DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
	DMA_InitStructure.DMA_Priority = DMA_Priority_High;
	DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
	DMA_Init(DMA1_Channel2, &DMA_InitStructure);

	//接收通道开启传输完成和传输错误中断
	DMA_ITConfig(DMA1_Channel2, DMA_IT_TC | DMA_IT_TE, ENABLE);

	//配置SPI1_TX：DMA1通道3
	DMA_DeInit(DMA1_Channel3);
	DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&(SPI1->DR);
	DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)&(MT6701_TxDummy);
	DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
	DMA_InitStructure.DMA_BufferSize = MT6701_FRAME_SIZE;
	DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
	DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Disable;
	DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
	DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
	DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
	DMA_InitStructure.DMA_Priority = DMA_Priority_High;
	DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
	DMA_Init(DMA1_Channel3, &DMA_InitStructure);

	//发送通道开启传输完成和传输错误中断
	DMA_ITConfig(DMA1_Channel3, DMA_IT_TC | DMA_IT_TE, ENABLE);

	//设置中断优先级分组
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

	//配置DMA1通道2中断
	NVIC_InitTypeDef NVIC_InitStructure;
	NVIC_InitStructure.NVIC_IRQChannel = DMA1_Channel2_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);

	//配置DMA1通道3中断
	NVIC_InitTypeDef NVIC_InitStructure;
	NVIC_InitStructure.NVIC_IRQChannel = DMA1_Channel3_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);

	//清除DMA通道的全部历史标志
	DMA_ClearFlag(DMA1_FLAG_GL2 | DMA1_FLAG_GL3);
}

/*********************DMA配置*/

/*内部辅助函数*********************/

/**
 * 函    数：清除SPI1溢出状态
 * 参    数：无
 * 返 回 值：无
 * 说    明：SPI1的DR寄存器在接收数据时会产生溢出，需手动清除
 */

static void MT6701_ClearOverrun(void)
{
	(void)SPI_I2S_ReceiveData(SPI1);
	(void)SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_OVR);
}

/**
  * 函    数：停止本次DMA通信
  * 参    数：无
  * 返 回 值：无
  */
static void MT6701_Stop(void)
{
	/*关闭SPI1的DMA请求*/
	SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Rx | SPI_I2S_DMAReq_Tx, DISABLE);

	/*关闭DMA通道*/
	DMA_Cmd(DMA1_Channel2, DISABLE);
	DMA_Cmd(DMA1_Channel3, DISABLE);

	/*等待SPI移位寄存器发送完最后一个时钟*/
	while(SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);

	/*满足最后一个时钟边沿到CSN上升沿的保持时间*/
	Delay_us(1);

	/*释放MT6701*/
	MT6701_W_CS(1);
	MT6701_Busy = 0;
}

/*********************内部辅助函数*/


/*用户接口*********************/

/**
  * 函    数：MT6701初始化
  * 参    数：无
  * 返 回 值：无
  */

void MT6701_Init(void)
{
	MT6701_GPIO_Init();
	MT6701_SPI_Init();
	MT6701_DMA_Init();

	/*初始化软件状态*/
	MT6701_Busy = 0;
	MT6701_DataReady = 0;
	MT6701_DMAError = 0;

	MT6701_ClearOverrun();
	MT6701_W_CS(1);
}

/**
  * 函    数：启动一次MT6701异步读取
  * 参    数：无
  * 返 回 值：1表示启动成功，0表示上一帧尚未完成
  * 说    明：函数启动DMA后立即返回，不会等待24位数据接收完成
  */
uint8_t MT6701_Start(void)
{
	/*上一次通信尚未完成时，不允许重复启动*/
	if (MT6701_Busy)
	{
		return 0;
	}

	/*确保DMA请求和通道处于关闭状态*/
	SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Rx | SPI_I2S_DMAReq_Tx, DISABLE);
	DMA_Cmd(DMA1_Channel2, DISABLE);
	DMA_Cmd(DMA1_Channel3, DISABLE);

	/*清除上一帧的DMA标志*/
	DMA_ClearFlag(DMA1_FLAG_GL2 | DMA1_FLAG_GL3);

	/*重新装载本次接收和发送长度*/
	DMA_SetCurrDataCounter(DMA1_Channel2, MT6701_FRAME_SIZE);
	DMA_SetCurrDataCounter(DMA1_Channel3, MT6701_FRAME_SIZE);
	
	/*清除旧状态*/
	MT6701_ClearOverrun();
	MT6701_DataReady = 0;
	MT6701_DMAError = 0;
	MT6701_Busy = 1;

	/*拉低CSN，开始一帧传输*/
	MT6701_W_CS(0);
	Delay_us(1);

	/*先开启接收通道，再开启发送通道产生时钟*/
	DMA_Cmd(DMA1_Channel2, ENABLE);
	DMA_Cmd(DMA1_Channel3, ENABLE);
	SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Rx | SPI_I2S_DMAReq_Tx, ENABLE);

	return 1;
}

/**
  * 函    数：获取MT6701通信忙状态
  * 参    数：无
  * 返 回 值：1表示正在通信，0表示空闲
  */
uint8_t MT6701_GetBusy(void)
{
	return MT6701_Busy;
}

/**
  * 函    数：获取MT6701 DMA通信错误状态
  * 参    数：无
  * 返 回 值：1表示DMA通信发生错误，0表示正常
  */
uint8_t MT6701_GetDMAError(void)
{
	return MT6701_DMAError;
}

/**
  * 函    数：计算MT6701的6位CRC
  * 参    数：Data 18位待校验数据，内容为14位角度和4位状态
  * 返 回 值：计算得到的6位CRC
  */
uint8_t MT6701_CalculateCRC(uint32_t Data)
{
	uint8_t CRCValue = 0;
	uint8_t Feedback;
	int8_t i;

	/*只保留有效的18位数据*/
	Data &= 0x3FFFF;

	/*CRC多项式：X^6 + X + 1，数据从最高位开始移入*/
	for (i = 17; i >= 0; i--)
	{
		Feedback = ((CRCValue >> 5) & 0x01) ^ ((Data >> i) & 0x01);
		CRCValue = (CRCValue << 1) & 0x3F; // 保留6位

		if (Feedback)
		{
			CRCValue ^= 0x03; // 多项式的低两位为11
		}
	}
	return CRCValue;
}

/**
  * 函    数：解析MT6701接收到的一帧数据
  * 参    数：Data 指向MT6701_Frame_t结构体的指针，用于存储解析结果
  * 返 回 值：MT6701_ReadResult_t 枚举类型，表示解析结果
  * 说    明：函数会从全局接收数组中提取角度、状态和CRC，并进行CRC校验
  */
MT6701_ReadResult_t MT6701_GetData(MT6701_Frame_t *Data)
{
	uint8_t RxData[MT6701_FRAME_SIZE];
	uint32_t Payload;

	/*参数为空或DMA尚未接收完成时，不读取数据*/
	if (Data == 0 || MT6701_DataReady == 0)
	{
		return MT6701_READ_NONE;
	}

	/*复制DMA接收结果，下一帧必须在本函数返回后才能启动*/
	RxData[0] = MT6701_RxData[0];
	RxData[1] = MT6701_RxData[1];
	RxData[2] = MT6701_RxData[2];
	MT6701_DataReady = 0;

	/*组合完整的24位原始数据*/
	Data->raw24 = ((uint32_t)RxData[0] << 16) |
				  ((uint32_t)RxData[1] << 8) |
				  (uint32_t)RxData[2];

	/*提取14位角度、4位状态和6位CRC*/
	Data->angle_raw = (Data->raw24 >> 10) & 0x3FFF;
	Data->status = (Data->raw24 >> 6) & 0x0F;
	Data->crc_received = Data->raw24 & 0x3F;

	/*CRC校验范围为帧的高18位*/
	Payload = Data->raw24 >> 6;
	Data->crc_calculated = MT6701_CalculateCRC(Payload);

	if (Data->crc_received != Data->crc_calculated)
	{
		return MT6701_READ_CRC_ERROR;
	}

	return MT6701_READ_OK;
}

/*********************用户接口*/


/*DMA中断处理*********************/

/**
  * 函    数：MT6701接收DMA中断处理
  * 参    数：无
  * 返 回 值：无
  * 说    明：由DMA1_Channel2_IRQHandler调用
  */
void MT6701_DMA_RxIRQHandler(void)
{
	/*接收DMA发生传输错误*/
	if (DMA_GetITStatus(DMA1_IT_TE2) == SET)
	{
		DMA_ClearITPendingBit(DMA1_IT_GL2);
		MT6701_DMAError = 1;
		MT6701_DataReady = 0;
		MT6701_Stop();
		return;
	}

}