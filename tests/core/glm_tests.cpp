#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/gtc/matrix_transform.hpp>

TEST(GLMTest, BasicVectorMultiplication) {
    glm::vec3 vec1(1.0f, 2.0f, 3.0f);
    glm::vec3 vec2(4.0f, 5.0f, 6.0f);

    glm::vec3 result = vec1 * vec2;

    glm::vec3 expected(4.0f, 10.0f, 18.0f);

    EXPECT_EQ(result.x, expected.x);
    EXPECT_EQ(result.y, expected.y);
    EXPECT_EQ(result.z, expected.z);
}

TEST(GLMTest, MatrixVectorMultiplication) {
    glm::mat4 matrix = glm::mat4(1.0f);

    glm::vec4 vec(1.0f, 2.0f, 3.0f, 1.0f);

    glm::vec4 result = matrix * vec;

    glm::vec4 expected(1.0f, 2.0f, 3.0f, 1.0f);

    EXPECT_EQ(result.x, expected.x);
    EXPECT_EQ(result.y, expected.y);
    EXPECT_EQ(result.z, expected.z);
    EXPECT_EQ(result.w, expected.w);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
