/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_MEDIATEK_MTK_MMSYS_H
#define __SOC_MEDIATEK_MTK_MMSYS_H

#define DISP_REG_CONFIG_DISP_OVL0_MOUT_EN	0x040
#define DISP_REG_CONFIG_DISP_OVL1_MOUT_EN	0x044
#define DISP_REG_CONFIG_DISP_OD_MOUT_EN		0x048
#define DISP_REG_CONFIG_DISP_GAMMA_MOUT_EN	0x04c
#define DISP_REG_CONFIG_DISP_UFOE_MOUT_EN	0x050
#define DISP_REG_CONFIG_DISP_COLOR0_SEL_IN	0x084
#define DISP_REG_CONFIG_DISP_COLOR1_SEL_IN	0x088
#define DISP_REG_CONFIG_DSIE_SEL_IN		0x0a4
#define DISP_REG_CONFIG_DSIO_SEL_IN		0x0a8
#define DISP_REG_CONFIG_DPI_SEL_IN		0x0ac
#define DISP_REG_CONFIG_DISP_RDMA2_SOUT		0x0b8
#define DISP_REG_CONFIG_DISP_RDMA0_SOUT_EN	0x0c4
#define DISP_REG_CONFIG_DISP_RDMA1_SOUT_EN	0x0c8
#define DISP_REG_CONFIG_MMSYS_CG_CON0		0x100

#define DISP_REG_CONFIG_DISP_OVL_MOUT_EN	0x030
#define DISP_REG_CONFIG_OUT_SEL			0x04c
#define DISP_REG_CONFIG_DSI_SEL			0x050
#define DISP_REG_CONFIG_DPI_SEL			0x064

#define OVL0_MOUT_EN_COLOR0			0x1
#define OD_MOUT_EN_RDMA0			0x1
#define OD1_MOUT_EN_RDMA1			BIT(16)
#define UFOE_MOUT_EN_DSI0			0x1
#define COLOR0_SEL_IN_OVL0			0x1
#define OVL1_MOUT_EN_COLOR1			0x1
#define GAMMA_MOUT_EN_RDMA1			0x1
#define RDMA0_SOUT_DPI0				0x2
#define RDMA0_SOUT_DPI1				0x3
#define RDMA0_SOUT_DSI1				0x1
#define RDMA0_SOUT_DSI2				0x4
#define RDMA0_SOUT_DSI3				0x5
#define RDMA0_SOUT_MASK				0x7
#define RDMA1_SOUT_DPI0				0x2
#define RDMA1_SOUT_DPI1				0x3
#define RDMA1_SOUT_DSI1				0x1
#define RDMA1_SOUT_DSI2				0x4
#define RDMA1_SOUT_DSI3				0x5
#define RDMA1_SOUT_MASK				0x7
#define RDMA2_SOUT_DPI0				0x2
#define RDMA2_SOUT_DPI1				0x3
#define RDMA2_SOUT_DSI1				0x1
#define RDMA2_SOUT_DSI2				0x4
#define RDMA2_SOUT_DSI3				0x5
#define RDMA2_SOUT_MASK				0x7
#define DPI0_SEL_IN_RDMA1			0x1
#define DPI0_SEL_IN_RDMA2			0x3
#define DPI0_SEL_IN_MASK			0x3
#define DPI1_SEL_IN_RDMA1			(0x1 << 8)
#define DPI1_SEL_IN_RDMA2			(0x3 << 8)
#define DPI1_SEL_IN_MASK			(0x3 << 8)
#define DSI0_SEL_IN_RDMA1			0x1
#define DSI0_SEL_IN_RDMA2			0x4
#define DSI0_SEL_IN_MASK			0x7
#define DSI1_SEL_IN_RDMA1			0x1
#define DSI1_SEL_IN_RDMA2			0x4
#define DSI1_SEL_IN_MASK			0x7
#define DSI2_SEL_IN_RDMA1			(0x1 << 16)
#define DSI2_SEL_IN_RDMA2			(0x4 << 16)
#define DSI2_SEL_IN_MASK			(0x7 << 16)
#define DSI3_SEL_IN_RDMA1			(0x1 << 16)
#define DSI3_SEL_IN_RDMA2			(0x4 << 16)
#define DSI3_SEL_IN_MASK			(0x7 << 16)
#define COLOR1_SEL_IN_OVL1			0x1

#define OVL_MOUT_EN_RDMA			0x1
#define BLS_TO_DSI_RDMA1_TO_DPI1		0x8
#define BLS_TO_DPI_RDMA1_TO_DSI			0x2
#define BLS_RDMA1_DSI_DPI_MASK			0xf
#define DSI_SEL_IN_BLS				0x0
#define DPI_SEL_IN_BLS				0x0
#define DPI_SEL_IN_MASK				0x1
#define DSI_SEL_IN_RDMA				0x1
#define DSI_SEL_IN_MASK				0x1

#define MMSYS_RST_NR(bank, bit) (((bank) * 32) + (bit))

/*
 * This macro adds a compile time check to make sure that the in/out
 * selection bit(s) fit in the register mask, similar to bitfield
 * macros, but this does not transform the value.
 */
#define MMSYS_ROUTE(from, to, reg_addr, reg_mask, selection)		\
	{ DDP_COMPONENT_##from, DDP_COMPONENT_##to, reg_addr, reg_mask,	\
	  (__BUILD_BUG_ON_ZERO_MSG((reg_mask) == 0, "Invalid mask") +	\
	   __BUILD_BUG_ON_ZERO_MSG(~(reg_mask) & (selection),		\
				   #selection " does not fit in "	\
				   #reg_mask) +				\
	   (selection))							\
	}

struct mtk_mmsys_routes {
	u32 from_comp;
	u32 to_comp;
	u32 addr;
	u32 mask;
	u32 val;
};

/**
 * struct mtk_mmsys_driver_data - Settings of the mmsys
 * @clk_driver: Clock driver name that the mmsys is using
 *              (defined in drivers/clk/mediatek/clk-*.c).
 * @routes: Routing table of the mmsys.
 *          It provides mux settings from one module to another.
 * @num_routes: Array size of the routes.
 * @sw0_rst_offset: Register offset for the reset control.
 * @num_resets: Number of reset bits that are defined
 * @is_vppsys: Whether the mmsys is VPPSYS (Video Processing Pipe)
 *             or VDOSYS (Video). Only VDOSYS needs to be added to drm driver.
 * @vsync_len: VSYNC length of the MIXER.
 *             VSYNC is usually triggered by the connector, so its length is a
 *             fixed value when the frame rate is decided, but ETHDR and
 *             MIXER generate their own VSYNC due to hardware design, therefore
 *             MIXER has to sync with ETHDR by adjusting VSYNC length.
 *             On MT8195, there is no such setting so we use the gap between
 *             falling edge and rising edge of SOF (Start of Frame) signal to
 *             do the job, but since MT8188, VSYNC_LEN setting is introduced to
 *             solve the problem and is given 0x40 (ticks) as the default value.
 *             Please notice that this value has to be set to 1 (minimum) if
 *             ETHDR is bypassed, otherwise MIXER could wait too long and causing
 *             underflow.
 *
 * Each MMSYS (multi-media system) may have different settings, they may use
 * different clock sources, mux settings, reset control ...etc., and these
 * differences are all stored here.
 */
struct mtk_mmsys_driver_data {
	const char *clk_driver;
	const struct mtk_mmsys_routes *routes;
	const unsigned int num_routes;
	const u16 sw0_rst_offset;
	const u8 *rst_tb;
	const u32 num_resets;
	const bool is_vppsys;
	const u8 vsync_len;
};

/*
 * Routes in mt2701 and mt2712 are different. That means
 * in the same register address, it controls different input/output
 * selection for each SoC. But, right now, they use the same table as
 * default routes meet their requirements. But we don't have the complete
 * route information for these three SoC, so just keep them in the same
 * table. After we've more information, we could separate mt2701, mt2712
 * to an independent table.
 */
static const struct mtk_mmsys_routes mmsys_default_routing_table[] = {
	{
		DDP_COMPONENT_BLS, DDP_COMPONENT_DSI0,
		DISP_REG_CONFIG_OUT_SEL, BLS_RDMA1_DSI_DPI_MASK,
		BLS_TO_DSI_RDMA1_TO_DPI1
	}, {
		DDP_COMPONENT_BLS, DDP_COMPONENT_DSI0,
		DISP_REG_CONFIG_DSI_SEL, DSI_SEL_IN_MASK,
		DSI_SEL_IN_BLS
	}, {
		DDP_COMPONENT_BLS, DDP_COMPONENT_DPI0,
		DISP_REG_CONFIG_OUT_SEL, BLS_RDMA1_DSI_DPI_MASK,
		BLS_TO_DPI_RDMA1_TO_DSI
	}, {
		DDP_COMPONENT_BLS, DDP_COMPONENT_DPI0,
		DISP_REG_CONFIG_DSI_SEL, DSI_SEL_IN_MASK,
		DSI_SEL_IN_RDMA
	}, {
		DDP_COMPONENT_BLS, DDP_COMPONENT_DPI0,
		DISP_REG_CONFIG_DPI_SEL, DPI_SEL_IN_MASK,
		DPI_SEL_IN_BLS
	}, {
		DDP_COMPONENT_GAMMA, DDP_COMPONENT_RDMA1,
		DISP_REG_CONFIG_DISP_GAMMA_MOUT_EN, GAMMA_MOUT_EN_RDMA1,
		GAMMA_MOUT_EN_RDMA1
	}, {
		DDP_COMPONENT_OD0, DDP_COMPONENT_RDMA0,
		DISP_REG_CONFIG_DISP_OD_MOUT_EN, OD_MOUT_EN_RDMA0,
		OD_MOUT_EN_RDMA0
	}, {
		DDP_COMPONENT_OD1, DDP_COMPONENT_RDMA1,
		DISP_REG_CONFIG_DISP_OD_MOUT_EN, OD1_MOUT_EN_RDMA1,
		OD1_MOUT_EN_RDMA1
	}, {
		DDP_COMPONENT_OVL0, DDP_COMPONENT_COLOR0,
		DISP_REG_CONFIG_DISP_OVL0_MOUT_EN, OVL0_MOUT_EN_COLOR0,
		OVL0_MOUT_EN_COLOR0
	}, {
		DDP_COMPONENT_OVL0, DDP_COMPONENT_COLOR0,
		DISP_REG_CONFIG_DISP_COLOR0_SEL_IN, COLOR0_SEL_IN_OVL0,
		COLOR0_SEL_IN_OVL0
	}, {
		DDP_COMPONENT_OVL0, DDP_COMPONENT_RDMA0,
		DISP_REG_CONFIG_DISP_OVL_MOUT_EN, OVL_MOUT_EN_RDMA,
		OVL_MOUT_EN_RDMA
	}, {
		DDP_COMPONENT_OVL1, DDP_COMPONENT_COLOR1,
		DISP_REG_CONFIG_DISP_OVL1_MOUT_EN, OVL1_MOUT_EN_COLOR1,
		OVL1_MOUT_EN_COLOR1
	}, {
		DDP_COMPONENT_OVL1, DDP_COMPONENT_COLOR1,
		DISP_REG_CONFIG_DISP_COLOR1_SEL_IN, COLOR1_SEL_IN_OVL1,
		COLOR1_SEL_IN_OVL1
	}, {
		DDP_COMPONENT_RDMA0, DDP_COMPONENT_DPI0,
		DISP_REG_CONFIG_DISP_RDMA0_SOUT_EN, RDMA0_SOUT_MASK,
		RDMA0_SOUT_DPI0
	}, {
		DDP_COMPONENT_RDMA0, DDP_COMPONENT_DPI1,
		DISP_REG_CONFIG_DISP_RDMA0_SOUT_EN, RDMA0_SOUT_MASK,
		RDMA0_SOUT_DPI1
	}, {
		DDP_COMPONENT_RDMA0, DDP_COMPONENT_DSI1,
		DISP_REG_CONFIG_DISP_RDMA0_SOUT_EN, RDMA0_SOUT_MASK,
		RDMA0_SOUT_DSI1
	}, {
		DDP_COMPONENT_RDMA0, DDP_COMPONENT_DSI2,
		DISP_REG_CONFIG_DISP_RDMA0_SOUT_EN, RDMA0_SOUT_MASK,
		RDMA0_SOUT_DSI2
	}, {
		DDP_COMPONENT_RDMA0, DDP_COMPONENT_DSI3,
		DISP_REG_CONFIG_DISP_RDMA0_SOUT_EN, RDMA0_SOUT_MASK,
		RDMA0_SOUT_DSI3
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DPI0,
		DISP_REG_CONFIG_DISP_RDMA1_SOUT_EN, RDMA1_SOUT_MASK,
		RDMA1_SOUT_DPI0
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DPI0,
		DISP_REG_CONFIG_DPI_SEL_IN, DPI0_SEL_IN_MASK,
		DPI0_SEL_IN_RDMA1
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DPI1,
		DISP_REG_CONFIG_DISP_RDMA1_SOUT_EN, RDMA1_SOUT_MASK,
		RDMA1_SOUT_DPI1
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DPI1,
		DISP_REG_CONFIG_DPI_SEL_IN, DPI1_SEL_IN_MASK,
		DPI1_SEL_IN_RDMA1
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DSI0,
		DISP_REG_CONFIG_DSIE_SEL_IN, DSI0_SEL_IN_MASK,
		DSI0_SEL_IN_RDMA1
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DSI1,
		DISP_REG_CONFIG_DISP_RDMA1_SOUT_EN, RDMA1_SOUT_MASK,
		RDMA1_SOUT_DSI1
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DSI1,
		DISP_REG_CONFIG_DSIO_SEL_IN, DSI1_SEL_IN_MASK,
		DSI1_SEL_IN_RDMA1
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DSI2,
		DISP_REG_CONFIG_DISP_RDMA1_SOUT_EN, RDMA1_SOUT_MASK,
		RDMA1_SOUT_DSI2
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DSI2,
		DISP_REG_CONFIG_DSIE_SEL_IN, DSI2_SEL_IN_MASK,
		DSI2_SEL_IN_RDMA1
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DSI3,
		DISP_REG_CONFIG_DISP_RDMA1_SOUT_EN, RDMA1_SOUT_MASK,
		RDMA1_SOUT_DSI3
	}, {
		DDP_COMPONENT_RDMA1, DDP_COMPONENT_DSI3,
		DISP_REG_CONFIG_DSIO_SEL_IN, DSI3_SEL_IN_MASK,
		DSI3_SEL_IN_RDMA1
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DPI0,
		DISP_REG_CONFIG_DISP_RDMA2_SOUT, RDMA2_SOUT_MASK,
		RDMA2_SOUT_DPI0
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DPI0,
		DISP_REG_CONFIG_DPI_SEL_IN, DPI0_SEL_IN_MASK,
		DPI0_SEL_IN_RDMA2
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DPI1,
		DISP_REG_CONFIG_DISP_RDMA2_SOUT, RDMA2_SOUT_MASK,
		RDMA2_SOUT_DPI1
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DPI1,
		DISP_REG_CONFIG_DPI_SEL_IN, DPI1_SEL_IN_MASK,
		DPI1_SEL_IN_RDMA2
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DSI0,
		DISP_REG_CONFIG_DSIE_SEL_IN, DSI0_SEL_IN_MASK,
		DSI0_SEL_IN_RDMA2
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DSI1,
		DISP_REG_CONFIG_DISP_RDMA2_SOUT, RDMA2_SOUT_MASK,
		RDMA2_SOUT_DSI1
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DSI1,
		DISP_REG_CONFIG_DSIO_SEL_IN, DSI1_SEL_IN_MASK,
		DSI1_SEL_IN_RDMA2
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DSI2,
		DISP_REG_CONFIG_DISP_RDMA2_SOUT, RDMA2_SOUT_MASK,
		RDMA2_SOUT_DSI2
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DSI2,
		DISP_REG_CONFIG_DSIE_SEL_IN, DSI2_SEL_IN_MASK,
		DSI2_SEL_IN_RDMA2
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DSI3,
		DISP_REG_CONFIG_DISP_RDMA2_SOUT, RDMA2_SOUT_MASK,
		RDMA2_SOUT_DSI3
	}, {
		DDP_COMPONENT_RDMA2, DDP_COMPONENT_DSI3,
		DISP_REG_CONFIG_DSIO_SEL_IN, DSI3_SEL_IN_MASK,
		DSI3_SEL_IN_RDMA2
	}, {
		DDP_COMPONENT_UFOE, DDP_COMPONENT_DSI0,
		DISP_REG_CONFIG_DISP_UFOE_MOUT_EN, UFOE_MOUT_EN_DSI0,
		UFOE_MOUT_EN_DSI0
	}
};

#endif /* __SOC_MEDIATEK_MTK_MMSYS_H */
