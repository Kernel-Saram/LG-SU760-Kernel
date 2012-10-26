#ifdef CONFIG_ARCH_OMAP2420#include "mux2420.h"#include "mux2430.h"#endif#ifdef CONFIG_ARCH_OMAP3#include "mux34xx.h"#endif#ifdef CONFIG_ARCH_OMAP4#include "mux44xx.h"#endif#define OMAP_MUX_TERMINATOR	0xffff#define OMAP_MUX_MODE_MASK  0x7#define OMAP_MUX_MODE0      0#define OMAP_MUX_MODE1      1#define OMAP_MUX_MODE2      2#define OMAP_MUX_MODE3      3#define OMAP_MUX_MODE4      4#define OMAP_MUX_MODE5      5#define OMAP_MUX_MODE6      6#define OMAP_MUX_MODE7      7#define OMAP_PULL_ENA			(1 << 3)#define OMAP_PULL_UP			(1 << 4)#define OMAP_ALTELECTRICALSEL		(1 << 5)#define OMAP_INPUT_EN			(1 << 8)#define OMAP_OFF_EN			(1 << 9)#define OMAP_OFFOUT_EN			(1 << 10)#define OMAP_OFFOUT_VAL			(1 << 11)#define OMAP_OFF_PULL_EN		(1 << 12)#define OMAP_OFF_PULL_UP		(1 << 13)#define OMAP_WAKEUP_EN			(1 << 14)#define OMAP_WAKEUP_EVENT		(1 << 15)#define OMAP_PIN_OUTPUT			0#define OMAP_PIN_INPUT			OMAP_INPUT_EN#define OMAP_PIN_INPUT_PULLUP		(OMAP_PULL_ENA | OMAP_INPUT_EN \						| OMAP_PULL_UP)#define OMAP_PIN_INPUT_PULLDOWN		(OMAP_PULL_ENA | OMAP_INPUT_EN)#define OMAP_PIN_OFF_NONE		0#define OMAP_PIN_OFF_OUTPUT_HIGH	(OMAP_OFF_EN | OMAP_OFFOUT_EN \						| OMAP_OFFOUT_VAL)#define OMAP_PIN_OFF_OUTPUT_LOW		(OMAP_OFF_EN | OMAP_OFFOUT_EN)#define OMAP_PIN_OFF_INPUT_PULLUP	(OMAP_OFF_EN | OMAP_OFF_PULL_EN \						| OMAP_OFF_PULL_UP)#define OMAP_PIN_OFF_INPUT_PULLDOWN	(OMAP_OFF_EN | OMAP_OFF_PULL_EN)#define OMAP_PIN_OFF_WAKEUPENABLE	OMAP_WAKEUP_EN#define OMAP_MODE_GPIO(x)	(((x) & OMAP_MUX_MODE7) == OMAP_MUX_MODE4)#define OMAP_PACKAGE_MASK		0xffff#define OMAP_PACKAGE_CBS		8		#define OMAP_PACKAGE_CBL		7		#define OMAP_PACKAGE_CBP		6		#define OMAP_PACKAGE_CUS		5		#define OMAP_PACKAGE_CBB		4		#define OMAP_PACKAGE_CBC		3		#define OMAP_PACKAGE_ZAC		2		#define OMAP_PACKAGE_ZAF		1		#define OMAP_MUX_NR_MODES		8		#define OMAP_MUX_NR_SIDES		2		#define OMAP_MUX_REG_8BIT		(1 << 0)#define OMAP_MUX_GPIO_IN_MODE3		(1 << 1)struct omap_mux_partition {	const char		*name;	u32			flags;	u32			phys;	u32			size;	void __iomem		*base;	struct list_head	muxmodes;	struct list_head	node;};struct omap_mux {	u16	reg_offset;	u16	gpio;#ifdef CONFIG_OMAP_MUX	char	*muxnames[OMAP_MUX_NR_MODES];#ifdef CONFIG_DEBUG_FS	char	*balls[OMAP_MUX_NR_SIDES];	struct omap_mux_partition *partition;#endif#endif};struct omap_ball {	u16	reg_offset;	char	*balls[OMAP_MUX_NR_SIDES];};struct omap_board_mux {	u16	reg_offset;	u16	value;};#if defined(CONFIG_OMAP_MUX)int omap_mux_init_gpio(int gpio, int val);int omap_mux_init_signal(const char *muxname, int val);u16 omap_mux_read_signal(const char *muxname); int omap_mux_enable_wakeup(const char *muxname);int omap_mux_disable_wakeup(const char *muxname); #elsestatic inline int omap_mux_init_gpio(int gpio, int val){	return 0;}static inline int omap_mux_init_signal(char *muxname, int val){	return 0;}static inline int omap_mux_enable_wakeup(char *muxname){	return 0;}static inline int omap_mux_disable_wakeup(char *muxname){	return 0;}#endifu16 omap_mux_get_gpio(int gpio);void omap_mux_set_gpio(u16 val, int gpio);int omap2420_mux_init(struct omap_board_mux *board_mux, int flags);int omap2430_mux_init(struct omap_board_mux *board_mux, int flags);int omap3_mux_init(struct omap_board_mux *board_mux, int flags);int omap4_mux_init(struct omap_board_mux *board_mux, int flags);int omap_mux_init(const char *name, u32 flags,		  u32 mux_pbase, u32 mux_size,		  struct omap_mux *superset,		  struct omap_mux *package_subset,		  struct omap_board_mux *board_mux,		  struct omap_ball *package_balls);