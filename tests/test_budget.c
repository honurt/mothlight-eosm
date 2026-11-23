/* Unit tests for the data-rate model and the profile parser (run on a PC: make -C sim test). */
#include <stdio.h>
#include <string.h>
#include "../modules/moth_ui/budget.h"
#include "../modules/moth_ui/profiles.h"

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static void test_budget(void)
{
    struct bpp_table t;
    budget_default_table(&t);

    /* 3K 2.35:1 10-bit lossless: 3072 x 1308 x 23.976 x 6.0 bit / 8 ~= 72.2 MB/s */
    struct rec_format k3 = { 3072, 1308, 23976, 10, 1 };
    int need = budget_kBps(&k3, &t);
    CHECK(need > 71500 && need < 73000);
    CHECK(budget_verdict(need, 65000) == BUDGET_BURST);
    CHECK(budget_verdict(need, 78000) == BUDGET_MARGINAL);

    /* 2.5K 10-bit is continuous on an overclocked card */
    struct rec_format k25 = { 2520, 1072, 23976, 10, 1 };
    CHECK(budget_verdict(budget_kBps(&k25, &t), 65000) == BUDGET_CONTINUOUS);

    /* uncompressed 14-bit is exactly 14 bit/px */
    struct rec_format raw14 = { 1736, 976, 23976, 14, 0 };
    CHECK(budget_bpp_x100(&raw14, &t) == 1400);

    /* the largest frame Crop Mood makes does not overflow */
    struct rec_format full = { 5208, 3478, 60000, 14, 0 };
    CHECK(budget_kBps(&full, &t) > 0);

    /* measuring: 10 s of 3K at exactly 6.0 bit/px */
    uint32_t kB = (uint32_t)((long long)3072 * 1308 * 6 / 8 * 23976 / 1000 * 10 / 1000);
    int m = budget_measure_bpp_x100(&k3, kB, 10000);
    CHECK(m >= 595 && m <= 605);
    CHECK(budget_measure_bpp_x100(&k3, kB, 1000) == 0);          /* too short */
    CHECK(budget_measure_bpp_x100(&k3, kB * 10, 10000) == 0);    /* 60 bit/px is not believable */

    budget_learn(&t, 10, 500);
    CHECK(t.x100[0] == 575);                                    /* (600*3 + 500) / 4 */

    CHECK(budget_minutes_on_card(60000, 60 * 1024) == 17);
    CHECK(budget_ratio_q8(65000, 65000) == 256);
    CHECK(budget_ratio_q8(200000, 65000) == 512);
}

static void test_profiles(void)
{
    struct rec_profile p[MAX_PROFILES];
    int n = profiles_builtin(p, MAX_PROFILES);
    CHECK(n >= 10);
    CHECK(strcmp(p[0].name, "3K Scope") == 0);
    CHECK(p[0].nominal.width == 3072 && p[0].nominal.height == 1308);   /* 3072/2.35, nearest even: Crop Mood's 1308 */
    CHECK(p[1].nominal.height == 1286);                                  /* 3K at 2.39 */

    for (int i = 0; i < n; i++)
    {
        if (p[i].mood == MOOD_1X3) CHECK(p[i].squeeze == 3);
        CHECK(p[i].nominal.width > 0 && p[i].nominal.height > 0);
    }

    const char * txt =
        "# my profiles\n"
        "Night 2.8K | 1:1 | 2.8K | 2.39 | 23.976 | 10\n"
        "\n"
        "Anamorphic | 1x3 | highest | 2.35 | 23.976 | 12   # comment\r\n"
        "Bad line   | 1:1 | 9K | 2.35 | 24 | 10\n"
        "Slow       | 3x3 | HFR  | 2.39 | 23.976 | 10\n"
        "Canon      | off |      |      |        |   \n";
    int bad = 0;
    struct rec_profile q[MAX_PROFILES];
    int m = profiles_parse(txt, q, MAX_PROFILES, &bad);
    CHECK(m == 4);
    CHECK(bad == 5);
    CHECK(strcmp(q[0].name, "Night 2.8K") == 0 && q[0].preset == P11_2_8K && q[0].ratio == AR_2_39);
    CHECK(q[1].mood == MOOD_1X3 && q[1].bits == BITS_12 && q[1].nominal.width == 1736 && q[1].nominal.height == 2214);
    CHECK(q[2].mood == MOOD_3X3 && q[2].nominal.fps_x1000 == 60000 && q[2].nominal.height == 694);
    CHECK(q[3].mood == MOOD_OFF);

    char buf[64];
    profile_summary(&p[0], buf, sizeof(buf));
    CHECK(strcmp(buf, "3072x1308  10-bit  23.976") == 0);
}

int main(void)
{
    test_budget();
    test_profiles();
    printf(fails ? "%d test(s) failed\n" : "all tests passed\n", fails);
    return fails != 0;
}
