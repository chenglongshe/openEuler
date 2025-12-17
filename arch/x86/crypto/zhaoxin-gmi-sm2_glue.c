// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SM2 asymmetric public-key algorithm
 * as specified by OSCCA GM/T 0003.1-2012 -- 0003.5-2012 SM2 and
 * described at https://tools.ietf.org/html/draft-shen-sm2-ecdsa-02
 *
 * Copyright (c) 2023 Shanghai Zhaoxin Semiconductor LTD.
 * Authors: YunShen <yunshen@zhaoxin.com>
 */

#include <linux/module.h>
#include <linux/mpi.h>
#include <crypto/internal/akcipher.h>
#include <crypto/akcipher.h>
#include <crypto/sm3.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>

#define DRIVER_VERSION "1.0.0"

#define SCRATCH_SIZE (4 * 2048)

asmlinkage int zx_gmi_sm2_verify(unsigned char *key, unsigned char *hash, unsigned char *sig,
				unsigned char *scratch);

struct sm4_cipher_data {
	u8 pub_key[65]; /* public key */
};

/* Load supported features of the CPU to see if the SM2 is available. */
static int zx_gmi_available(void)
{
	struct cpuinfo_x86 *c = &cpu_data(0);

	if (((c->x86 == 6) && (c->x86_model >= 0x0f)) || (c->x86 > 6)) {
		if (!boot_cpu_has(X86_FEATURE_SM2) || !boot_cpu_has(X86_FEATURE_SM2_EN)) {
			pr_err("can't enable hardware SM2 if ZX-GMI-SM2 is not enabled\n");
			return -ENODEV;
		}
		pr_info("This cpu support ZX-GMI-SM2\n");
		return 0;
	}
	return -ENODEV;
}
static int zx_sm2_ec_ctx_init(void)
{
	return zx_gmi_available();
}

/* Zhaoxin sm2 verify function */
static int _zx_sm2_verify(struct sm4_cipher_data *ec, unsigned char *hash, unsigned char *sig)
{
	int ret = -EINVAL;
	uint64_t f_ok = 0;
	unsigned char *scratch = kmalloc(SCRATCH_SIZE, GFP_KERNEL);

	memset(scratch, 0, SCRATCH_SIZE);
	f_ok = zx_gmi_sm2_verify(ec->pub_key, hash, sig, scratch);
	if (f_ok == 1)
		ret = 0;
	else
		ret = -EKEYREJECTED;

	kfree(scratch);
	return ret;
}

static void zx_sm2_ec_ctx_deinit(struct sm4_cipher_data *ec)
{
	memset(ec, 0, sizeof(*ec));
}

static int zx_sm2_verify(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct sm4_cipher_data *ec = akcipher_tfm_ctx(tfm);
	unsigned char *buffer;
	int ret;

	buffer = kmalloc(req->src_len + req->dst_len, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	sg_pcopy_to_buffer(req->src,
		sg_nents_for_len(req->src, req->src_len + req->dst_len),
		buffer, req->src_len + req->dst_len, 0);

	ret = _zx_sm2_verify(ec, buffer + req->src_len, buffer);
	kfree(buffer);
	return ret;
}

static int zx_sm2_set_pub_key(struct crypto_akcipher *tfm, const void *key, unsigned int keylen)
{
	int rc = 0;
	struct sm4_cipher_data *ec = akcipher_tfm_ctx(tfm);

	memcpy(ec->pub_key, key, keylen);
	return rc;
}

static unsigned int zx_sm2_max_size(struct crypto_akcipher *tfm)
{
	/* Unlimited max size */
	return PAGE_SIZE;
}

static int zx_sm2_init_tfm(struct crypto_akcipher *tfm)
{
	return zx_sm2_ec_ctx_init();
}

static void zx_sm2_exit_tfm(struct crypto_akcipher *tfm)
{
	struct sm4_cipher_data *ec = akcipher_tfm_ctx(tfm);

	zx_sm2_ec_ctx_deinit(ec);
}

static struct akcipher_alg zx_sm2 = {
	.verify = zx_sm2_verify,
	.set_pub_key = zx_sm2_set_pub_key,
	.max_size = zx_sm2_max_size,
	.init = zx_sm2_init_tfm,
	.exit = zx_sm2_exit_tfm,
	.base = {
		.cra_name = "sm2",
		.cra_driver_name = "zhaoxin-gmi-sm2",
		.cra_priority = 150,
		.cra_module = THIS_MODULE,
		.cra_ctxsize = sizeof(struct sm4_cipher_data),
	},
};

static int __init zx_sm2_init(void)
{
	return crypto_register_akcipher(&zx_sm2);
}

static void __exit zx_sm2_exit(void)
{
	crypto_unregister_akcipher(&zx_sm2);
}

module_init(zx_sm2_init);
module_exit(zx_sm2_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR("YunShen <yunshen@zhaoxin.com>");
MODULE_DESCRIPTION("Zhaoxin GMI SM2 Algorithm");
MODULE_ALIAS_CRYPTO("zx-gmi-sm2");
