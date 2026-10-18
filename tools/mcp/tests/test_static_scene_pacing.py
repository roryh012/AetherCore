import unittest
from verify_static_scene_pacing import reports,assess

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

if __name__=='__main__':unittest.main()
