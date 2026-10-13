#include "Query/LMDBBenchmark.h"

#include <gtest/gtest.h>

TEST(LMDBBenchmarkTest, SyntheticQualificationQueriesAndBatchComplete)
{
    const auto result = VNX::LMDBBenchmark::runSynthetic(1000);

    EXPECT_TRUE(result.complete);
    EXPECT_EQ(result.objectCount, 1000);
    EXPECT_GT(result.wordResultCount, 0);
    EXPECT_GT(result.phraseResultCount, 0);
    EXPECT_GT(result.trigramResultCount, 0);
    EXPECT_EQ(result.boundedBatchCount, 256);
}
