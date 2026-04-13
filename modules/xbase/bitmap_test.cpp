/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bitmap_test.cpp - xBitmap unit tests
 */

#include <gtest/gtest.h>

#include <cstring>

extern "C" {
#include <xbase/bitmap.h>
}

/* ═══════════════════ Lifecycle ═══════════════════ */

TEST(BitmapTest, InitAndFree) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 64), xErrno_Ok);
  EXPECT_EQ(bm.nbits, 64u);
  EXPECT_EQ(bm.nbytes, 8u);
  EXPECT_TRUE(bm.owned);
  EXPECT_NE(bm.data, nullptr);
  xBitmapFree(&bm);
  EXPECT_EQ(bm.data, nullptr);
  EXPECT_EQ(bm.nbits, 0u);
}

TEST(BitmapTest, InitNullBitmap) {
  EXPECT_EQ(xBitmapInit(nullptr, 64), xErrno_InvalidArg);
}

TEST(BitmapTest, InitZeroBits) {
  xBitmap bm;
  EXPECT_EQ(xBitmapInit(&bm, 0), xErrno_InvalidArg);
}

TEST(BitmapTest, FreeNull) {
  xBitmapFree(nullptr); /* should not crash */
}

TEST(BitmapTest, InitStatic) {
  uint8_t buf[4] = {0};
  xBitmap bm;
  ASSERT_EQ(xBitmapInitStatic(&bm, buf, 4, 30), xErrno_Ok);
  EXPECT_EQ(bm.nbits, 30u);
  EXPECT_EQ(bm.nbytes, 4u);
  EXPECT_FALSE(bm.owned);
  EXPECT_EQ(bm.data, buf);
  xBitmapFree(&bm); /* should not free buf */
}

TEST(BitmapTest, InitStaticBufferTooSmall) {
  uint8_t buf[2];
  xBitmap bm;
  /* 17 bits need 3 bytes, but only 2 provided */
  EXPECT_EQ(xBitmapInitStatic(&bm, buf, 2, 17), xErrno_InvalidArg);
}

TEST(BitmapTest, InitStaticNullData) {
  xBitmap bm;
  EXPECT_EQ(xBitmapInitStatic(&bm, nullptr, 4, 30), xErrno_InvalidArg);
}

/* ═══════════════════ Single-bit operations ═══════════════════ */

TEST(BitmapTest, SetAndTest) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 100), xErrno_Ok);

  /* All bits should start as 0 */
  for (uint32_t i = 0; i < 100; i++) {
    EXPECT_FALSE(xBitmapTest(&bm, i)) << "bit " << i;
  }

  /* Set some bits */
  xBitmapSet(&bm, 0);
  xBitmapSet(&bm, 7);
  xBitmapSet(&bm, 8);
  xBitmapSet(&bm, 63);
  xBitmapSet(&bm, 99);

  EXPECT_TRUE(xBitmapTest(&bm, 0));
  EXPECT_TRUE(xBitmapTest(&bm, 7));
  EXPECT_TRUE(xBitmapTest(&bm, 8));
  EXPECT_TRUE(xBitmapTest(&bm, 63));
  EXPECT_TRUE(xBitmapTest(&bm, 99));

  /* Unset bits should still be 0 */
  EXPECT_FALSE(xBitmapTest(&bm, 1));
  EXPECT_FALSE(xBitmapTest(&bm, 50));
  EXPECT_FALSE(xBitmapTest(&bm, 98));

  xBitmapFree(&bm);
}

TEST(BitmapTest, Clear) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 32), xErrno_Ok);

  xBitmapSet(&bm, 10);
  EXPECT_TRUE(xBitmapTest(&bm, 10));

  xBitmapClear(&bm, 10);
  EXPECT_FALSE(xBitmapTest(&bm, 10));

  xBitmapFree(&bm);
}

TEST(BitmapTest, Toggle) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 16), xErrno_Ok);

  EXPECT_FALSE(xBitmapTest(&bm, 5));
  xBitmapToggle(&bm, 5);
  EXPECT_TRUE(xBitmapTest(&bm, 5));
  xBitmapToggle(&bm, 5);
  EXPECT_FALSE(xBitmapTest(&bm, 5));

  xBitmapFree(&bm);
}

TEST(BitmapTest, OutOfBoundsIsNoOp) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 10), xErrno_Ok);

  /* These should be safe no-ops */
  xBitmapSet(&bm, 10);
  xBitmapSet(&bm, 100);
  xBitmapClear(&bm, 10);
  xBitmapToggle(&bm, 10);
  EXPECT_FALSE(xBitmapTest(&bm, 10));
  EXPECT_FALSE(xBitmapTest(&bm, 100));

  xBitmapFree(&bm);
}

TEST(BitmapTest, NullBitmapOps) {
  /* Should not crash */
  xBitmapSet(nullptr, 0);
  xBitmapClear(nullptr, 0);
  xBitmapToggle(nullptr, 0);
  EXPECT_FALSE(xBitmapTest(nullptr, 0));
}

/* ═══════════════════ Bulk operations ═══════════════════ */

TEST(BitmapTest, SetAll) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 20), xErrno_Ok);

  xBitmapSetAll(&bm);
  for (uint32_t i = 0; i < 20; i++) {
    EXPECT_TRUE(xBitmapTest(&bm, i)) << "bit " << i;
  }

  xBitmapFree(&bm);
}

TEST(BitmapTest, ClearAll) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 20), xErrno_Ok);

  xBitmapSetAll(&bm);
  xBitmapClearAll(&bm);
  for (uint32_t i = 0; i < 20; i++) {
    EXPECT_FALSE(xBitmapTest(&bm, i)) << "bit " << i;
  }

  xBitmapFree(&bm);
}

TEST(BitmapTest, Count) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 100), xErrno_Ok);

  EXPECT_EQ(xBitmapCount(&bm), 0u);

  xBitmapSet(&bm, 0);
  xBitmapSet(&bm, 50);
  xBitmapSet(&bm, 99);
  EXPECT_EQ(xBitmapCount(&bm), 3u);

  xBitmapSetAll(&bm);
  EXPECT_EQ(xBitmapCount(&bm), 100u);

  xBitmapClear(&bm, 0);
  EXPECT_EQ(xBitmapCount(&bm), 99u);

  xBitmapFree(&bm);
}

TEST(BitmapTest, CountNull) {
  EXPECT_EQ(xBitmapCount(nullptr), 0u);
}

TEST(BitmapTest, Full) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 20), xErrno_Ok);

  EXPECT_FALSE(xBitmapFull(&bm));

  xBitmapSetAll(&bm);
  EXPECT_TRUE(xBitmapFull(&bm));

  xBitmapClear(&bm, 19);
  EXPECT_FALSE(xBitmapFull(&bm));

  xBitmapFree(&bm);
}

TEST(BitmapTest, FullAlignedSize) {
  /* 8-bit aligned: no tail bits */
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 16), xErrno_Ok);

  xBitmapSetAll(&bm);
  EXPECT_TRUE(xBitmapFull(&bm));

  xBitmapClear(&bm, 0);
  EXPECT_FALSE(xBitmapFull(&bm));

  xBitmapFree(&bm);
}

TEST(BitmapTest, Empty) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 20), xErrno_Ok);

  EXPECT_TRUE(xBitmapEmpty(&bm));

  xBitmapSet(&bm, 5);
  EXPECT_FALSE(xBitmapEmpty(&bm));

  xBitmapClear(&bm, 5);
  EXPECT_TRUE(xBitmapEmpty(&bm));

  xBitmapFree(&bm);
}

TEST(BitmapTest, EmptyNull) {
  EXPECT_TRUE(xBitmapEmpty(nullptr));
}

TEST(BitmapTest, FullNull) {
  EXPECT_FALSE(xBitmapFull(nullptr));
}

/* ═══════════════════ Serialisation ═══════════════════ */

TEST(BitmapTest, Data) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 16), xErrno_Ok);

  xBitmapSet(&bm, 0);  /* byte 0, bit 0 → 0x01 */
  xBitmapSet(&bm, 8);  /* byte 1, bit 0 → 0x01 */

  uint32_t nbytes = 0;
  const uint8_t *data = xBitmapData(&bm, &nbytes);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(nbytes, 2u);
  EXPECT_EQ(data[0], 0x01);
  EXPECT_EQ(data[1], 0x01);

  xBitmapFree(&bm);
}

TEST(BitmapTest, DataNull) {
  uint32_t nbytes = 42;
  const uint8_t *data = xBitmapData(nullptr, &nbytes);
  EXPECT_EQ(data, nullptr);
  EXPECT_EQ(nbytes, 0u);
}

TEST(BitmapTest, DataNullNbytes) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 8), xErrno_Ok);
  /* Should not crash when nbytes is NULL */
  const uint8_t *data = xBitmapData(&bm, nullptr);
  EXPECT_NE(data, nullptr);
  xBitmapFree(&bm);
}

/* ═══════════════════ Edge cases ═══════════════════ */

TEST(BitmapTest, SingleBit) {
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 1), xErrno_Ok);
  EXPECT_EQ(bm.nbytes, 1u);

  EXPECT_TRUE(xBitmapEmpty(&bm));
  EXPECT_FALSE(xBitmapFull(&bm));

  xBitmapSet(&bm, 0);
  EXPECT_TRUE(xBitmapFull(&bm));
  EXPECT_FALSE(xBitmapEmpty(&bm));
  EXPECT_EQ(xBitmapCount(&bm), 1u);

  xBitmapFree(&bm);
}

TEST(BitmapTest, NonAlignedBits) {
  /* 13 bits → 2 bytes, 5 trailing bits unused */
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, 13), xErrno_Ok);
  EXPECT_EQ(bm.nbytes, 2u);

  xBitmapSetAll(&bm);
  EXPECT_TRUE(xBitmapFull(&bm));
  EXPECT_EQ(xBitmapCount(&bm), 13u);

  /* Bit 12 is the last valid bit */
  EXPECT_TRUE(xBitmapTest(&bm, 12));
  /* Bit 13 is out of bounds */
  EXPECT_FALSE(xBitmapTest(&bm, 13));

  xBitmapFree(&bm);
}

TEST(BitmapTest, LargeBitmap) {
  /* Simulate a 1GB file with 64KB chunks → 16384 chunks → 2KB bitmap */
  const uint32_t nbits = 16384;
  xBitmap bm;
  ASSERT_EQ(xBitmapInit(&bm, nbits), xErrno_Ok);
  EXPECT_EQ(bm.nbytes, 2048u);

  /* Set every other bit */
  for (uint32_t i = 0; i < nbits; i += 2) {
    xBitmapSet(&bm, i);
  }
  EXPECT_EQ(xBitmapCount(&bm), nbits / 2);

  /* Set all remaining bits */
  for (uint32_t i = 1; i < nbits; i += 2) {
    xBitmapSet(&bm, i);
  }
  EXPECT_TRUE(xBitmapFull(&bm));
  EXPECT_EQ(xBitmapCount(&bm), nbits);

  xBitmapFree(&bm);
}

TEST(BitmapTest, StaticBitmapModification) {
  uint8_t buf[4] = {0};
  xBitmap bm;
  ASSERT_EQ(xBitmapInitStatic(&bm, buf, 4, 32), xErrno_Ok);

  xBitmapSet(&bm, 0);
  xBitmapSet(&bm, 31);

  /* Verify the external buffer was modified */
  EXPECT_EQ(buf[0], 0x01);
  EXPECT_EQ(buf[3], 0x80);

  EXPECT_TRUE(xBitmapTest(&bm, 0));
  EXPECT_TRUE(xBitmapTest(&bm, 31));
  EXPECT_EQ(xBitmapCount(&bm), 2u);

  xBitmapFree(&bm); /* should not free buf */
  /* buf should still be intact */
  EXPECT_EQ(buf[0], 0x01);
}

TEST(BitmapTest, ExactByteAligned) {
  /* Exactly 8, 16, 24, 32 bits — no tail */
  for (uint32_t nbits : {8u, 16u, 24u, 32u}) {
    xBitmap bm;
    ASSERT_EQ(xBitmapInit(&bm, nbits), xErrno_Ok);
    EXPECT_EQ(bm.nbytes, nbits / 8);

    xBitmapSetAll(&bm);
    EXPECT_TRUE(xBitmapFull(&bm));
    EXPECT_EQ(xBitmapCount(&bm), nbits);

    xBitmapClearAll(&bm);
    EXPECT_TRUE(xBitmapEmpty(&bm));
    EXPECT_EQ(xBitmapCount(&bm), 0u);

    xBitmapFree(&bm);
  }
}
