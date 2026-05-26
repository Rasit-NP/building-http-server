# include <gtest/gtest.h>
# include "../src/calculator.h"

TEST(CalculatorTest, AddPositiveNumbers)
{
    EXPECT_EQ(add(2, 3), 5);
    EXPECT_EQ(add(3, 4), 7);
}

TEST(CalculatorTest, AddNegativeNumbers)
{
    EXPECT_EQ(add(-2, -1), -3);
    EXPECT_EQ(add(-3, 4), 1);
}