import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('extract_affinity', Path(__file__).resolve().parents[1]/'tools/extract_dialogue_affinity.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ExtractionTests(unittest.TestCase):
    def test_dialogue_dispatch_and_nested_conditions(self):
        # Distinguish direct dialogue events from delayed quest-stage rewards.
        source = '''Function HandleDialogueBump_DialogueDrumlinDiner(Quest eventQuest, Int eventID)
  If eventQuest == DialogueDrumlinDiner
    If eventID == 5
      Self.Cait_Loves(True)
      Self.Codsworth_Dislikes(True)
    ElseIf eventID == 6
      If SomeQuest.GetStage() > 10
        Self.Cait_Likes(True)
      Else
        Self.Cait_Dislikes(True)
      EndIf
    EndIf
  EndIf
EndFunction
Function HandleQuestStageBump_Test(Quest eventQuest, Int eventID)
  If eventID == 5
    Self.Cait_Hates(True)
  EndIf
EndFunction'''
        events = module.event_reactions(source)
        self.assertEqual(events[('dialoguedrumlindiner', 5)], [
            {'companion': 'Cait', 'reaction': 'loves', 'conditional': False},
            {'companion': 'Codsworth', 'reaction': 'dislikes', 'conditional': False}])
        self.assertEqual({v['reaction'] for v in events[('dialoguedrumlindiner', 6)]}, {'likes', 'dislikes'})
        self.assertTrue(all(v['conditional'] for v in events[('dialoguedrumlindiner', 6)]))
        self.assertNotIn(('test', 5), events)

    def test_truncated_and_extended_records(self):
        with self.assertRaises(ValueError):
            list(module.records(b'INFO'))
        payload = b'x'*70000
        raw = b'XXXX'+struct.pack('<HI', 4, len(payload))+b'VMAD'+struct.pack('<H', 0)+payload
        self.assertEqual(list(module.subrecords(raw)), [(b'VMAD', payload)])


if __name__ == '__main__':
    unittest.main()
