// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt) "qcom-prime-helper: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/platform_device.h>
#include <soc/qcom/prime_helper.h>

static atomic_t trigger_done = ATOMIC_INIT(0);

static int qcom_prime_helper_probe(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "Prime Online Helper probe\n");
	return 0;
}

static void qcom_prime_helper_release(struct device *dev)
{
	/* statically allocated device, nothing to free */
}

static struct platform_device qcom_prime_helper_device = {
	.name = "qcom-prime-helper",
	.id = -1,
	.dev = {
		.release = qcom_prime_helper_release,
	},
};

static struct platform_driver qcom_prime_helper_driver = {
	.probe = qcom_prime_helper_probe,
	.driver = {
		.name = "qcom-prime-helper",
	},
};

void prime_helper_trigger(void)
{
	int ret;

	/* one-shot, safe against concurrent callers */
	if (atomic_xchg(&trigger_done, 1))
		return;

	ret = platform_device_register(&qcom_prime_helper_device);
	if (ret)
		pr_err("Prime Online Helper Device Register Fail: %d\n", ret);
	else
		pr_info("Prime Online Helper Device Register Succeed\n");
}
EXPORT_SYMBOL(prime_helper_trigger);

static int prime_helper_cpu_online(unsigned int cpu)
{
	if (cpu == CPU_PRIME_ISOLATE)
		prime_helper_trigger();
	return 0;
}

static int __init qcom_prime_helper_init(void)
{
	int ret;

	ret = platform_driver_register(&qcom_prime_helper_driver);
	if (ret)
		return ret;

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"qcom/prime_helper:online",
					prime_helper_cpu_online, NULL);
	if (ret < 0) {
		pr_err("failed to register CPU hotplug callback: %d\n", ret);
		platform_driver_unregister(&qcom_prime_helper_driver);
		return ret;
	}

	/* The prime CPU may already be up by the time we get here */
	if (cpu_online(CPU_PRIME_ISOLATE))
		prime_helper_trigger();

	return 0;
}
module_init(qcom_prime_helper_init);

MODULE_DESCRIPTION("QCOM Prime Core Helper Driver");
MODULE_LICENSE("GPL v2");