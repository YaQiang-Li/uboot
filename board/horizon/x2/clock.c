
#include <asm/io.h>
#include <asm/arch/x2_sysctrl.h>
#include <asm/arch/clock.h>
#include <linux/delay.h>


#ifdef CONFIG_SPL_BUILD
void dram_pll_init(ulong pll_val)
{
	unsigned int value;
	unsigned int try_num = 5;

	writel(PD_BIT | DSMPD_BIT | FOUTPOST_DIV_BIT | FOUTVCO_BIT, X2_DDRPLL_PD_CTRL);

	switch (pll_val) {
		case MHZ(3200):
			/* Set DDR PLL to 1600 */
			value = FBDIV_BITS(200) | REFDIV_BITS(3) |
				POSTDIV1_BITS(1) | POSTDIV2_BITS(1);
			writel(value, X2_DDRPLL_FREQ_CTRL);

			writel(0x1, X2_DDRSYS_CLK_DIV_SEL);

			break;

		case MHZ(2133):
			/* Set DDR PLL to 2133 */
			value = FBDIV_BITS(87) | REFDIV_BITS(1) |
				POSTDIV1_BITS(2) | POSTDIV2_BITS(1);
			writel(value, X2_DDRPLL_FREQ_CTRL);

			writel(0x1, X2_DDRSYS_CLK_DIV_SEL);

			break;

		default:
			break;
	}

	value = readl(X2_DDRPLL_PD_CTRL);
	value &= ~(PD_BIT | FOUTPOST_DIV_BIT);
	writel(value, X2_DDRPLL_PD_CTRL);

	while (!(value = readl(X2_DDRPLL_STATUS) & LOCK_BIT)) {
		if (try_num <= 0) {
			break;
		}

		udelay(100);
		try_num--;
	}

	value = readl(X2_PLLCLK_SEL);
	value |= DDRCLK_SEL_BIT;
	writel(value, X2_PLLCLK_SEL);

	writel(0x1, X2_DDRSYS_CLKEN_SET);

	return;
}
#endif /* CONFIG_SPL_BUILD */

