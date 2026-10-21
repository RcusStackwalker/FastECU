#ifdef FASTECU_EXPECT_POINTER_BYTES
static_assert(sizeof(void *) == FASTECU_EXPECT_POINTER_BYTES);
#endif

namespace fastecu::qualification
{
int TransitionValue()
{
    return 0;
}
} // namespace fastecu::qualification
