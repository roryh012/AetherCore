import unittest
from verify_static_scene_pacing import reports,assess,phase_reports

class StaticPacingTests(unittest.TestCase):
 def test_report_and_regression_thresholds(self):
  line='FrameReport n=600 avg=16.67 median=16.67 p95=16.8 p99=17.1 max=19.0 | game=0.21 inflight=0.0 present=0.8 inputstale=0.0 latch2flip=0/0 cap=15 pacer=0 tail=0 | flipPeriod=16 flips=1 slack=2 paced=0 missed=0 est=1 vrr=0 | clamped=0 unthrottled=0 focused=0 focusflips=0'
  values=reports(line)
  self.assertEqual(values[0]['n'],600)
  self.assertEqual(values[0]['p99'],17.1)
  self.assertTrue(all(x['passed'] for x in assess(values,values).values()))
  self.assertFalse(assess(values,[dict(values[0],p99=100)])['p99']['passed'])
  self.assertFalse(assess(values,[dict(values[0],max=100)])['max']['passed'])
  self.assertEqual(reports('FrameReport malformed'),[])

 def test_actual_mode_and_phase_windows(self):
  line='FramePhases n=600 mode=FIFO requested=MAILBOX mean/max | render=3.1/4.2 begin=0.1/0.2 pump=0.2/0.3 update=1.0/2.0 ui=5.0/30.0 extract=0.5/0.9 sceneui=0.1/0.2 submit=0.01/0.02 uibegin=1.0/2.0 uibuild=3.0/20.0 uifinal=1.0/8.0 fence=2.0/4.0 acquire=0.1/0.3 presentcall=0.2/0.5 | BACKGROUND FIFO: foreground pacing unverified'
  row=phase_reports(line)[0]
  self.assertEqual((row['mode'],row['requested']),('FIFO','MAILBOX'))
  self.assertEqual(row['phases']['ui'],dict(mean=5.0,max=30.0))
  self.assertEqual(row['phases']['uibuild'],dict(mean=3.0,max=20.0))
  self.assertEqual(row['phases']['fence'],dict(mean=2.0,max=4.0))
  self.assertEqual(row['phases']['presentcall'],dict(mean=0.2,max=0.5))
  self.assertEqual(len(row['phases']),14)
  self.assertEqual(phase_reports('FramePhases malformed'),[])

if __name__=='__main__':unittest.main()
