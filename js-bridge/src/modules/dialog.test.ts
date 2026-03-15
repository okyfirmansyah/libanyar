// ---------------------------------------------------------------------------
// @libanyar/api/dialog — unit tests
// ---------------------------------------------------------------------------

import { describe, it, expect, vi, beforeEach } from 'vitest';
import { open, save, message, ask, confirm } from './dialog';

vi.mock('../invoke', () => ({
  invoke: vi.fn(),
}));

import { invoke } from '../invoke';
const mockInvoke = vi.mocked(invoke);

describe('dialog module', () => {
  beforeEach(() => {
    mockInvoke.mockReset();
  });

  describe('open', () => {
    it('invokes dialog:open with options', async () => {
      mockInvoke.mockResolvedValue(['/home/user/file.txt']);
      const result = await open({
        title: 'Pick a file',
        filters: [{ name: 'Text', extensions: ['txt'] }],
        multiple: false,
      });
      expect(result).toEqual(['/home/user/file.txt']);
      expect(mockInvoke).toHaveBeenCalledWith('dialog:open', {
        title: 'Pick a file',
        filters: [{ name: 'Text', extensions: ['txt'] }],
        multiple: false,
      });
    });

    it('returns null when cancelled', async () => {
      mockInvoke.mockResolvedValue(null);
      const result = await open();
      expect(result).toBeNull();
    });
  });

  describe('save', () => {
    it('invokes dialog:save with options', async () => {
      mockInvoke.mockResolvedValue('/home/user/output.txt');
      const result = await save({ defaultPath: '/home/user/output.txt' });
      expect(result).toBe('/home/user/output.txt');
      expect(mockInvoke).toHaveBeenCalledWith('dialog:save', {
        defaultPath: '/home/user/output.txt',
      });
    });
  });

  describe('message', () => {
    it('invokes dialog:message with text', async () => {
      mockInvoke.mockResolvedValue('Ok');
      const result = await message('Operation complete');
      expect(result).toBe('Ok');
      expect(mockInvoke).toHaveBeenCalledWith('dialog:message', {
        message: 'Operation complete',
      });
    });

    it('accepts string as title shorthand', async () => {
      mockInvoke.mockResolvedValue('Ok');
      await message('Hello', 'My Title');
      expect(mockInvoke).toHaveBeenCalledWith('dialog:message', {
        title: 'My Title',
        message: 'Hello',
      });
    });

    it('accepts full options object', async () => {
      mockInvoke.mockResolvedValue('Yes');
      await message('Discard?', {
        kind: 'warning',
        buttons: 'YesNo',
      });
      expect(mockInvoke).toHaveBeenCalledWith('dialog:message', {
        kind: 'warning',
        buttons: 'YesNo',
        message: 'Discard?',
      });
    });

    it('supports custom button labels', async () => {
      mockInvoke.mockResolvedValue('Yes');
      await message('Save?', {
        buttons: { yes: 'Save', no: 'Discard', cancel: 'Cancel' },
      });
      expect(mockInvoke).toHaveBeenCalledWith('dialog:message', {
        buttons: { yes: 'Save', no: 'Discard', cancel: 'Cancel' },
        message: 'Save?',
      });
    });
  });

  describe('ask', () => {
    it('invokes dialog:ask and returns boolean', async () => {
      mockInvoke.mockResolvedValue(true);
      const result = await ask('Delete this?');
      expect(result).toBe(true);
      expect(mockInvoke).toHaveBeenCalledWith('dialog:ask', {
        message: 'Delete this?',
      });
    });

    it('accepts string as title shorthand', async () => {
      mockInvoke.mockResolvedValue(false);
      await ask('Question?', 'Title');
      expect(mockInvoke).toHaveBeenCalledWith('dialog:ask', {
        title: 'Title',
        message: 'Question?',
      });
    });
  });

  describe('confirm', () => {
    it('invokes dialog:confirm and returns boolean', async () => {
      mockInvoke.mockResolvedValue(true);
      const result = await confirm('Proceed?', {
        title: 'Confirmation',
        kind: 'warning',
      });
      expect(result).toBe(true);
      expect(mockInvoke).toHaveBeenCalledWith('dialog:confirm', {
        title: 'Confirmation',
        kind: 'warning',
        message: 'Proceed?',
      });
    });

    it('accepts custom button labels', async () => {
      mockInvoke.mockResolvedValue(false);
      await confirm('Discard?', {
        okLabel: 'Yes, discard',
        cancelLabel: 'Keep editing',
      });
      expect(mockInvoke).toHaveBeenCalledWith('dialog:confirm', {
        okLabel: 'Yes, discard',
        cancelLabel: 'Keep editing',
        message: 'Discard?',
      });
    });
  });
});
