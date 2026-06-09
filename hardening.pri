# Bounds-check hardening — see docs/superpowers/specs/2026-06-09-bounds-check-hardening-design.md
#
# Re-arm Q_ASSERT even in release (QT_NO_DEBUG) so every QByteArray/QString/QList
# .at()/operator[] overrun aborts loudly with file:line instead of silent UB.
DEFINES += QT_FORCE_ASSERTS

# Opt-in AddressSanitizer + UndefinedBehaviorSanitizer build for the residue Qt
# asserts cannot see (raw memory, std::, use-after-free). Enable with:
#   qmake6 <project>.pro CONFIG+=sanitize
sanitize {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
    QMAKE_LFLAGS   += -fsanitize=address,undefined
}
