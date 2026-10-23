using System;
using System.IO;
using System.Runtime.InteropServices;
using Harken;
using NUnit.Framework;
using UnityEngine;

// This assembly is deliberately absent from every InternalsVisibleTo list.
public class ExternalConsumerTests
{
#if HARKEN_SDK_TEST_NULL_PUMP
    [DllImport("HarkenCore", CallingConvention = CallingConvention.Cdecl)]
    private static extern VnResult HarkenSdkTest_PumpNull(uint blocks);
    [DllImport("HarkenCore", CallingConvention = CallingConvention.Cdecl)]
    private static extern VnResult HarkenSdkTest_QueueNotification();

    [Test]
    public void PublicHostPreservesCallbacksAndTerminalActionRelease()
    {
        Assert.That(typeof(HarkenEngine).Assembly, Is.Not.EqualTo(GetType().Assembly));
        string root = Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
        VnConfig config = HarkenEngine.DefaultConfig();
        config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
        object marker = new object();
        int hostThread = System.Threading.Thread.CurrentThread.ManagedThreadId;
        int notifications = 0;
        bool callbackStateValid = true;
        using (HarkenCallbacks.Registration registration = HarkenCallbacks.InstallNotification(ref config,
            (in VnNotification value, object state) =>
            {
                callbackStateValid &= ReferenceEquals(marker, state) &&
                    hostThread == System.Threading.Thread.CurrentThread.ManagedThreadId;
                ++notifications;
            }, marker))
        {
            GC.Collect(); GC.WaitForPendingFinalizers();
            Assert.That(PackBootstrap.StartDirectory(root, null, ref config, out PackBootstrapSession session),
                Is.EqualTo(VnResult.VN_OK));
            using (session)
            {
                Assert.That(session.PrepareActions(new[] { "Play_U4_A" }, session.ProjectId,
                    out ActionPreparation prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Emitter.Create(out Emitter emitter), Is.EqualTo(VnResult.VN_OK));
                Assert.That(emitter.SetPosition(new VnVec3 { x = 1 }), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Listener.Set(default, new VnVec3 { z = 1 }, new VnVec3 { y = 1 }), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Post("Play_U4_A", emitter, out ActionFiring firing), Is.EqualTo(VnResult.VN_OK));
                // Allow the authored start to pass through admission and audio-start acknowledgement.
                for (int block = 0; block < 8; ++block)
                {
                    Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                }
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(firing.Seek(100, VnActionSeekMode.VN_ACTION_SEEK_ABSOLUTE, out ActionSeekTicket seek),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(seek.IsValid, Is.True);
                Assert.That(seek.Query(out VnActionSeekInfoV1 accepted), Is.EqualTo(VnResult.VN_OK));
                Assert.That(accepted.firing, Is.EqualTo(firing.Handle));
                Assert.That(accepted.state, Is.EqualTo((uint)VnActionSeekState.VN_ACTION_SEEK_ACCEPTED));
                Assert.That(accepted.targetCount, Is.GreaterThan(0));
                Assert.That(seek.Release(), Is.EqualTo(VnResult.VN_IN_USE));
                Assert.That(firing.Seek(200, VnActionSeekMode.VN_ACTION_SEEK_ABSOLUTE, out ActionSeekTicket refused),
                    Is.EqualTo(VnResult.VN_IN_USE));
                Assert.That(refused.IsValid, Is.False);
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                Assert.That(seek.Query(out VnActionSeekInfoV1 applied), Is.EqualTo(VnResult.VN_OK));
                Assert.That(applied.acceptedLogicSequence, Is.EqualTo(accepted.acceptedLogicSequence));
                Assert.That(applied.state, Is.EqualTo((uint)VnActionSeekState.VN_ACTION_SEEK_APPLIED));
                Assert.That(applied.appliedCount, Is.EqualTo(accepted.targetCount));
                Assert.That(applied.appliedAudioBlockSequence, Is.GreaterThan(accepted.appliedAudioBlockSequence));
                Assert.That(applied.minimumAppliedSourceFrame, Is.EqualTo(4800));
                Assert.That(seek.Release(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(seek.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(firing.Seek(double.NaN, VnActionSeekMode.VN_ACTION_SEEK_ABSOLUTE, out refused),
                    Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(refused.IsValid, Is.False);
                Assert.That(firing.Stop(0), Is.EqualTo(VnResult.VN_OK));
                bool terminal = false;
                for (int i = 0; i < 1024 && !terminal; ++i)
                {
                    Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
                    terminal = info.state == (uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED;
                }
                Assert.That(terminal, Is.True, "Stop must reach a releasable terminal state.");
                Assert.That(firing.Release(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(emitter.Destroy(), Is.EqualTo(VnResult.VN_OK));
                int before = notifications;
                Assert.That(HarkenSdkTest_QueueNotification(), Is.EqualTo(VnResult.VN_OK));
                GC.Collect(); GC.WaitForPendingFinalizers();
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(notifications, Is.GreaterThan(before));
                Assert.That(callbackStateValid, Is.True);
                Assert.That(prepared.Release(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.Shutdown(), Is.EqualTo(VnResult.VN_OK));
            }
            registration.Dispose(); registration.Dispose();
            Assert.That(registration.IsActive, Is.False);
        }
    }
#endif
}
