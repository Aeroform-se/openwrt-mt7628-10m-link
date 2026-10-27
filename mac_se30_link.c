/*
 * mac_se30_link.c - OpenWrt Kernel Module to force and PERSIST MT7628 PHY at 10Mbps Half-Duplex
 *
 * Designed for TP-Link TL-WR902AC v3 (MediaTek MT7628AN / OpenWrt 23.05.5)
 * to connect to legacy Macintosh network cards (e.g. Asante MacCon 10Base-T on SE/30).
 *
 * Hardware switch: Embedded Fast Ethernet Switch (ESW)
 * Base physical address: 0x10110000
 * Registers:
 *   PCR0 (offset 0xC0) - PHY Control Register 0 (Data, Command, Reg, PHY)
 *   PCR1 (offset 0xC4) - PHY Control Register 1 (Read Data, Status flags)
 *
 * Operation:
 *   Runs as a resident kernel module with a background monitor workqueue.
 *   Continuously checks PHY 4 (external LAN port) every 1 second.
 *   If the kernel switch driver (esw_rt3050), network reload, or cable reconnect
 *   resets the PHY to 100Mbps Auto-negotiation (0x3100), this module immediately
 *   re-enforces 10Mbps Half-Duplex (BMCR=0x0000) and restricts advertisement
 *   (ADVERTISE=0x0021) so the link never reverts to 100Mbps.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>

#define DRV_NAME            "mac_se30_link"

/* MediaTek MT7628 Embedded Switch (ESW) physical base */
#define MT7628_ESW_BASE     0x10110000
#define MT7628_ESW_SIZE     0x1000

/* Switch PHY Control Registers (PCR0 & PCR1) */
#define RT305X_ESW_REG_PCR0 0x00c0
#define RT305X_ESW_REG_PCR1 0x00c4

/* PCR0 bit fields */
#define PCR0_DATA_SHIFT     16
#define PCR0_RD_PHY_CMD     BIT(14)
#define PCR0_WT_PHY_CMD     BIT(13)
#define PCR0_REG_SHIFT      8
#define PCR0_PHY_SHIFT      0

/* PCR1 bit fields */
#define PCR1_RD_DATA_SHIFT  16
#define PCR1_RD_RDY         BIT(1)
#define PCR1_WT_DONE        BIT(0)

/* Standard MII Registers */
#define MII_BMCR            0x00    /* Basic Mode Control Register */
#define MII_BMSR            0x01    /* Basic Mode Status Register */
#define MII_PHYSID1         0x02    /* PHY Identifier 1 */
#define MII_PHYSID2         0x03    /* PHY Identifier 2 */
#define MII_ADVERTISE       0x04    /* Auto-Negotiation Advertisement Register */
#define MII_LPA             0x05    /* Auto-Negotiation Link Partner Ability */

/* BMSR bits */
#define BMSR_LSTATUS        BIT(2)  /* Link status (1 = link up) */
#define BMSR_ANEGCOMPLETE   BIT(5)  /* Auto-negotiation complete */

/* Target configuration */
#define TARGET_PHY_PORT     4       /* Physical external LAN port on TL-WR902AC v3 */
#define BMCR_10M_HALF       0x0000  /* 10Mbps, Half Duplex, Auto-Negotiation Disabled */
#define ANAR_10M_HALF_ONLY  0x0021  /* Selector 802.3 (0x01) | 10Base-T Half (0x20) */

#define MDIO_TIMEOUT_MS     100

static int poll_interval_ms = 1000;
module_param(poll_interval_ms, int, 0644);
MODULE_PARM_DESC(poll_interval_ms, "PHY register monitor polling interval in ms (default: 1000)");

static void __iomem *sw_base;
static struct delayed_work monitor_dwork;
static int last_link_state = -1;

/*
 * esw_mii_read - Read a 16-bit register from the internal switch PHY
 */
static u16 esw_mii_read(void __iomem *base, u32 phy, u32 reg)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(MDIO_TIMEOUT_MS);
	u32 val;

	/* Trigger read command: reg address, phy address, and read command bit */
	iowrite32(((reg & 0x1f) << PCR0_REG_SHIFT) |
	          ((phy & 0x1f) << PCR0_PHY_SHIFT) |
	          PCR0_RD_PHY_CMD,
	          base + RT305X_ESW_REG_PCR0);

	/* Poll for RD_RDY in PCR1 */
	while (time_before(jiffies, timeout)) {
		val = ioread32(base + RT305X_ESW_REG_PCR1);
		if (val & PCR1_RD_RDY)
			return (val >> PCR1_RD_DATA_SHIFT) & 0xffff;
		cpu_relax();
	}

	pr_warn("%s: MDIO read timeout (PHY %u, Reg %u)\n", DRV_NAME, phy, reg);
	return 0xffff;
}

/*
 * esw_mii_write - Write a 16-bit register to the internal switch PHY
 */
static int esw_mii_write(void __iomem *base, u32 phy, u32 reg, u16 data)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(MDIO_TIMEOUT_MS);

	/* Ensure any prior write has finished (WT_DONE clear) */
	while (time_before(jiffies, timeout)) {
		if (!(ioread32(base + RT305X_ESW_REG_PCR1) & PCR1_WT_DONE))
			break;
		cpu_relax();
	}

	/* Issue write command: data payload, reg, phy, and write command bit */
	iowrite32(((u32)data << PCR0_DATA_SHIFT) |
	          ((reg & 0x1f) << PCR0_REG_SHIFT) |
	          ((phy & 0x1f) << PCR0_PHY_SHIFT) |
	          PCR0_WT_PHY_CMD,
	          base + RT305X_ESW_REG_PCR0);

	/* Wait for WT_DONE to be asserted */
	timeout = jiffies + msecs_to_jiffies(MDIO_TIMEOUT_MS);
	while (time_before(jiffies, timeout)) {
		if (ioread32(base + RT305X_ESW_REG_PCR1) & PCR1_WT_DONE)
			return 0;
		cpu_relax();
	}

	pr_err("%s: MDIO write timeout (PHY %u, Reg %u, Data 0x%04x)\n",
	       DRV_NAME, phy, reg, data);
	return -ETIMEDOUT;
}

/*
 * apply_10m_half_duplex - Enforce 10Mbps Half-Duplex on target PHY
 */
static void apply_10m_half_duplex(void __iomem *base, u32 phy)
{
	/* Select local register page (0x8000) */
	esw_mii_write(base, phy, 31, 0x8000);

	/* Restrict advertised capabilities to 10Base-T Half-Duplex only */
	esw_mii_write(base, phy, MII_ADVERTISE, ANAR_10M_HALF_ONLY);

	/* Force 10Mbps, Half-Duplex, Auto-Negotiation Disabled (0x0000) */
	esw_mii_write(base, phy, MII_BMCR, BMCR_10M_HALF);
}

/*
 * link_monitor_work_fn - Periodic timer worker that verifies and re-enforces link state
 */
static void link_monitor_work_fn(struct work_struct *work)
{
	u16 bmcr, anar, bmsr;
	int current_link;

	if (!sw_base)
		return;

	bmcr = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_BMCR);
	anar = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_ADVERTISE);
	bmsr = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_BMSR);
	current_link = (bmsr & BMSR_LSTATUS) ? 1 : 0;

	/* If BMCR was reset back to 0x3100 or ANAR restored, re-enforce immediately */
	if (bmcr != BMCR_10M_HALF || anar != ANAR_10M_HALF_ONLY) {
		pr_info("%s: Detected PHY %u reset (BMCR=0x%04x, ANAR=0x%04x). Re-enforcing 10M Half...\n",
		        DRV_NAME, TARGET_PHY_PORT, bmcr, anar);
		apply_10m_half_duplex(sw_base, TARGET_PHY_PORT);

		/* Re-read after applying */
		bmcr = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_BMCR);
		bmsr = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_BMSR);
		current_link = (bmsr & BMSR_LSTATUS) ? 1 : 0;
	}

	/* Log link status changes */
	if (current_link != last_link_state) {
		last_link_state = current_link;
		pr_info("%s: Port 4 physical link is now %s (BMCR=0x%04x, BMSR=0x%04x)\n",
		        DRV_NAME, current_link ? "UP" : "DOWN", bmcr, bmsr);
	}

	/* Reschedule next check */
	schedule_delayed_work(&monitor_dwork, msecs_to_jiffies(poll_interval_ms));
}

static int __init mac_link_init(void)
{
	u16 bmcr_before, anar_before, bmsr_before;
	u16 bmcr_after, anar_after, bmsr_after;

	pr_info("%s: Initializing MediaTek MT7628 persistent PHY link monitor (poll=%dms)...\n",
	        DRV_NAME, poll_interval_ms);

	sw_base = ioremap(MT7628_ESW_BASE, MT7628_ESW_SIZE);
	if (!sw_base) {
		pr_err("%s: Failed to ioremap switch memory at 0x%08x\n",
		       DRV_NAME, MT7628_ESW_BASE);
		return -ENOMEM;
	}

	bmcr_before = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_BMCR);
	anar_before = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_ADVERTISE);
	bmsr_before = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_BMSR);

	pr_info("%s: [PHY 4 Before] BMCR=0x%04x, ANAR=0x%04x, BMSR=0x%04x (Link:%s)\n",
	        DRV_NAME, bmcr_before, anar_before, bmsr_before,
	        (bmsr_before & BMSR_LSTATUS) ? "UP" : "DOWN");

	/* Apply 10Mbps Half-Duplex configuration */
	apply_10m_half_duplex(sw_base, TARGET_PHY_PORT);
	udelay(100);

	bmcr_after = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_BMCR);
	anar_after = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_ADVERTISE);
	bmsr_after = esw_mii_read(sw_base, TARGET_PHY_PORT, MII_BMSR);
	last_link_state = (bmsr_after & BMSR_LSTATUS) ? 1 : 0;

	pr_info("%s: [PHY 4 After]  BMCR=0x%04x, ANAR=0x%04x, BMSR=0x%04x (Link:%s)\n",
	        DRV_NAME, bmcr_after, anar_after, bmsr_after,
	        last_link_state ? "UP" : "DOWN");

	/* Initialize and start periodic delayed workqueue */
	INIT_DELAYED_WORK(&monitor_dwork, link_monitor_work_fn);
	schedule_delayed_work(&monitor_dwork, msecs_to_jiffies(poll_interval_ms));

	pr_info("%s: Link monitor active. PHY 4 locked to 10Mbps Half-Duplex.\n", DRV_NAME);
	return 0;
}

static void __exit mac_link_exit(void)
{
	pr_info("%s: Unloading module. Stopping link monitor...\n", DRV_NAME);

	/* Cancel background monitor work synchronously */
	cancel_delayed_work_sync(&monitor_dwork);

	if (sw_base) {
		/* Restore default 100M Full-Duplex Auto-negotiation on unload */
		pr_info("%s: Restoring default 100M Auto-negotiation on PHY %u...\n",
		        DRV_NAME, TARGET_PHY_PORT);
		esw_mii_write(sw_base, TARGET_PHY_PORT, 31, 0x8000);
		esw_mii_write(sw_base, TARGET_PHY_PORT, MII_ADVERTISE, 0x01e1);
		esw_mii_write(sw_base, TARGET_PHY_PORT, MII_BMCR, 0x3100);

		iounmap(sw_base);
		sw_base = NULL;
	}

	pr_info("%s: Module successfully unloaded.\n", DRV_NAME);
}

module_init(mac_link_init);
module_exit(mac_link_exit);

MODULE_AUTHOR("Antigravity Pair Programming");
MODULE_DESCRIPTION("Persistent 10Mbps Half-Duplex Enforcer for MT7628 / Mac SE/30");
MODULE_LICENSE("GPL");
MODULE_VERSION("2.0");
