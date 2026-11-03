#pragma once

#include <QByteArray>
#include <QtGlobal>

/**
 * @file
 * @brief Hides a real Tailscale installation from the code under test.
 *
 * core::Tailscale::resolveExecutable() falls back deliberately: a configured path
 * that is not runnable is a misconfiguration, and the useful response is to look in
 * the usual places rather than to pretend nothing is installed. That behaviour is
 * correct and is tested on its own.
 *
 * It does mean that any test asking "what happens when Tailscale is not installed?"
 * is really asking about the machine it runs on. Two tests did exactly that and
 * passed for a year, because no developer machine had Tailscale on it — then one did,
 * and they began failing with no code change behind them.
 *
 * The dangerous half of that is the direction of the error. Continuous integration
 * runners have no Tailscale either, so the pipeline would have stayed green while the
 * author's own machine went red, and the obvious conclusion — "CI is right, my
 * machine is odd" — is the wrong one.
 *
 * Emptying PATH removes the only step in the fallback that can reach outside the
 * build directory. Step two looks beside the test executable, which holds the
 * fixtures and no tailscale binary, so resolution genuinely finds nothing and the
 * test asks its intended question on every machine.
 *
 * Scoped to one test function at a time, because an empty PATH is not a safe thing
 * to leave behind for the suites that do launch processes.
 */
class NoTailscaleOnPath
{
public:
    NoTailscaleOnPath()
        : m_savedPath(qgetenv("PATH"))
    {
        qputenv("PATH", QByteArray());
    }

    ~NoTailscaleOnPath() { qputenv("PATH", m_savedPath); }

    NoTailscaleOnPath(const NoTailscaleOnPath&)            = delete;
    NoTailscaleOnPath& operator=(const NoTailscaleOnPath&) = delete;

private:
    QByteArray m_savedPath;
};
