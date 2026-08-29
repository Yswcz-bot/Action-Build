// SPDX-License-Identifier: GPL-2.0-only
/*
 * splash_probe: OnePlus Ace 5 Pro (SM8750) cont_splash region live-write probe
 *
 * Iteration 1 goals:
 *   1. dump mode: read back splash_region memory, analyze pixel format
 *      (raw RGB vs DSC stream) and stride
 *   2. write mode: write a simple animated test pattern to the top of the
 *      splash buffer; DPU scans this buffer continuously during cont_splash,
 *      so the change should appear live on the panel
 *
 * Verified device facts:
 *   - splash_region: pa=0xfc800000, size=0x2b00000 (43 MiB)
 *   - cont_splash enabled at ~9.3s in boot (dmesg)
 *   - panel: 1264x2780 DSI cmd mode + DSC (AA590_P_3_A0020_dsc_cmd)
 *   - kernel 6.6.147-android15-8-...-4k, clang 18 r510928, KCFI on
 *
 * Params:
 *   pa=0xFC800000   splash region physical address
 *   size=0x2B00000  region size
 *   mode=dump|write dump: analyze only; write: animated pattern
 *   interval=500    frame interval in ms (write mode)
 *   restore=1       restore original content on unload
 *
 * Build against OnePlus GKI 6.6.118 (android15-8 KMI), clang 18 r510928.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>

#define SPLASH_PA_DEFAULT   0xFC800000UL
#define SPLASH_SIZE_DEFAULT 0x2B00000UL

static ulong pa = SPLASH_PA_DEFAULT;
module_param(pa, ulong, 0644);
MODULE_PARM_DESC(pa, "splash region physical address");

static ulong size = SPLASH_SIZE_DEFAULT;
module_param(size, ulong, 0644);
MODULE_PARM_DESC(size, "splash region size");

static char *mode = "dump";
module_param(mode, charp, 0644);
MODULE_PARM_DESC(mode, "dump | write");

static int interval = 500;
module_param(interval, int, 0644);
MODULE_PARM_DESC(interval, "frame interval ms (write mode)");

static bool restore = true;
module_param(restore, bool, 0644);
MODULE_PARM_DESC(restore, "restore original content on unload");

static void __iomem *vaddr;
static u8 *backup;
static size_t backup_len;
static struct task_struct *anim_thread;

/* Panel geometry guess: 1264 wide, cmd mode, RGB8888 typical splash stride */
#define PANEL_W		1264
#define TEST_H		240	/* rows we touch at top of buffer */

static void dump_region(void)
{
	u32 w[8];
	int i;
	size_t nonzero = 0, zero = 0;
	const size_t sample_step = 4; /* read every 4th byte over first 16K */
	const size_t n = min_t(size_t, size, SZ_16K);

	pr_info("splash_probe: dump mode pa=0x%lx size=0x%lx\n", pa, size);

	for (i = 0; i < 8; i++)
		w[i] = readl(vaddr + i * 4);
	pr_info("splash_probe: first 8 words: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);

	for (i = 0; i < 4096; i++) {
		u8 b = readb(vaddr + (size_t)i * sample_step);
		if (b)
			nonzero++;
		else
			zero++;
	}
	pr_info("splash_probe: 4096-byte sample (step=4): nonzero=%zu zero=%zu\n",
		nonzero, zero);

	/* Try to detect image vs compressed stream: compare adjacent rows
	 * at several stride guesses. Real RGB image: rows differ smoothly.
	 * DSC stream: row-major relationship meaningless. */
	{
		static const int strides[] = { 1264 * 4, 1264 * 3, 1280 * 4,
					       2780 * 4, 2780 * 3 };
		int s;

		for (s = 0; s < ARRAY_SIZE(strides); s++) {
			int stride = strides[s];
			int same = 0, diff = 0, j;
			int rows = 0;

			for (i = 0; i < 48 && (size_t)(i + 1) * stride < n; i++) {
				int d = 0;
				rows++;
				for (j = 0; j < stride; j += 32) {
					u8 a = readb(vaddr + (size_t)i * stride + j);
					u8 b = readb(vaddr + (size_t)(i + 1) * stride + j);
					if (a != b)
						d++;
				}
				if (d)
					diff++;
				else
					same++;
			}
			pr_info("splash_probe: stride=%-6d rows=%d same=%d diff=%d\n",
				stride, rows, same, diff);
		}
	}

	print_hex_dump(KERN_INFO, "splash_probe hdr: ", DUMP_PREFIX_OFFSET,
		       16, 1, (void __force *)vaddr, 256, true);
	pr_info("splash_probe: dump complete\n");
}

/* Write animated test pattern to top TEST_H rows.
 * frame % 3 selects base color; a white line sweeps down; every 8th column
 * gets a grey marker to expose stride/pitch and byte order. */
static void write_pattern(int frame)
{
	int x, y;
	static const u32 colors[3] = { 0x0000FF, 0x00FF00, 0xFF0000 };
	u32 base = colors[frame % 3];
	int line = frame % TEST_H;

	for (y = 0; y < TEST_H; y++) {
		u32 c = (y == line) ? 0xFFFFFF : base;

		for (x = 0; x < PANEL_W; x++) {
			u32 v = c;
			if ((x & 0x7) == 0)
				v = 0x808080;
			writel(v, vaddr + ((size_t)y * PANEL_W + x) * 4);
		}
	}
}

static int anim_loop(void *data)
{
	int frame = 0;

	pr_info("splash_probe: write mode started, interval=%dms\n", interval);
	while (!kthread_should_stop()) {
		write_pattern(frame);
		frame++;
		if (msleep_interruptible(interval))
			break;
	}
	pr_info("splash_probe: write mode stopped (frames=%d)\n", frame);
	return 0;
}

static int __init splash_probe_init(void)
{
	size_t map_size;

	vaddr = ioremap_wc(pa, size);
	if (!vaddr) {
		vaddr = ioremap(pa, size);
		if (!vaddr) {
			pr_err("splash_probe: ioremap(0x%lx, 0x%lx) failed\n",
			       pa, size);
			return -ENOMEM;
		}
	}
	pr_info("splash_probe: mapped pa=0x%lx size=0x%lx -> va=%px (wc=%d)\n",
		pa, size, vaddr, 1);

	if (!strcmp(mode, "dump")) {
		dump_region();
		iounmap(vaddr);
		vaddr = NULL;
		return 0;
	}

	if (strcmp(mode, "write")) {
		pr_err("splash_probe: unknown mode '%s' (dump|write)\n", mode);
		iounmap(vaddr);
		vaddr = NULL;
		return -EINVAL;
	}

	/* write mode: backup the region we are going to touch */
	backup_len = (size_t)TEST_H * PANEL_W * 4;
	map_size = min_t(size_t, backup_len, size);
	backup = vzalloc(backup_len);
	if (!backup) {
		iounmap(vaddr);
		vaddr = NULL;
		return -ENOMEM;
	}
	memcpy_fromio(backup, vaddr, map_size);
	pr_info("splash_probe: backed up %zu bytes\n", map_size);

	anim_thread = kthread_run(anim_loop, NULL, "splash_anim");
	if (IS_ERR(anim_thread)) {
		pr_err("splash_probe: kthread_run failed\n");
		vfree(backup);
		backup = NULL;
		iounmap(vaddr);
		vaddr = NULL;
		return PTR_ERR(anim_thread);
	}

	return 0;
}

static void __exit splash_probe_exit(void)
{
	if (anim_thread) {
		kthread_stop(anim_thread);
		anim_thread = NULL;
	}
	if (vaddr) {
		if (backup && restore)
			memcpy_toio(vaddr, backup,
				    min_t(size_t, backup_len, size));
		iounmap(vaddr);
		vaddr = NULL;
	}
	if (backup) {
		vfree(backup);
		backup = NULL;
	}
	pr_info("splash_probe: unloaded\n");
}

module_init(splash_probe_init);
module_exit(splash_probe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("eta");
MODULE_DESCRIPTION("OnePlus Ace 5 Pro cont_splash region probe/live-write");
