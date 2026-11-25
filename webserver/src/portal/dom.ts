import type {
  BottomSheetField,
  CardField,
  NetworkListField,
  NetworkStatusField,
  ValidatableField,
} from './types';

function getRequiredElement<T>(id: string): T {
  const element = document.getElementById(id);
  if (!element) {
    throw new Error(`Required portal element "#${id}" was not found.`);
  }

  return element as T;
}

export interface PortalDom {
  followupLogoEl: HTMLDivElement;
  // WiFi
  wifiStatusCard: NetworkStatusField;
  wifiSettingsSheet: BottomSheetField;
  wifiSettingsCloseBtn: HTMLElement;
  wifiSettingsNotification: HTMLDivElement;
  networkList: NetworkListField;
  passwordInput: ValidatableField;
  scanBtn: HTMLButtonElement;
  connectBtn: HTMLButtonElement;
  // LocalAi API key
  transcriptionUrlInput: ValidatableField;
  transcriptionModelInput: ValidatableField;
  ollamaUrlInput: ValidatableField;
  ollamaModelInput: ValidatableField;
  localAiCard: CardField;
  localAiApiKeyInput: ValidatableField & { readOnly: boolean };
  localAiClearBtn: HTMLButtonElement;
  localAiSaveBtn: HTMLButtonElement;
  localAiTokenClearBtn: HTMLButtonElement;
  // Time / timezone
  timezoneLocationCard: CardField;
  timezoneSelect: ValidatableField;
  manualTimeInput: ValidatableField;
  manualDateInput: ValidatableField;
  timezoneLocationClearBtn: HTMLButtonElement;
  timezoneLocationSaveBtn: HTMLButtonElement;
}

export function createPortalDom(): PortalDom {
  return {
    followupLogoEl: getRequiredElement<HTMLDivElement>('followupLogo'),

    wifiStatusCard: getRequiredElement<NetworkStatusField>('wifiStatusCard'),
    wifiSettingsSheet: getRequiredElement<BottomSheetField>('wifiSettingsSheet'),
    wifiSettingsCloseBtn: getRequiredElement<HTMLElement>('wifiSettingsCloseBtn'),
    wifiSettingsNotification: getRequiredElement<HTMLDivElement>('wifiSettingsNotification'),
    networkList: getRequiredElement<NetworkListField>('networkList'),
    passwordInput: getRequiredElement<ValidatableField>('password'),
    scanBtn: getRequiredElement<HTMLButtonElement>('scanBtn'),
    connectBtn: getRequiredElement<HTMLButtonElement>('connectBtn'),

    transcriptionUrlInput: getRequiredElement<ValidatableField>('transcriptionUrlInput'),
    transcriptionModelInput: getRequiredElement<ValidatableField>('transcriptionModelInput'),
    ollamaUrlInput: getRequiredElement<ValidatableField>('ollamaUrlInput'),
    ollamaModelInput: getRequiredElement<ValidatableField>('ollamaModelInput'),
    localAiCard: getRequiredElement<CardField>('localAiCard'),
    localAiApiKeyInput: getRequiredElement<ValidatableField & { readOnly: boolean }>(
      'localAiApiKeyInput'
    ),
    localAiClearBtn: getRequiredElement<HTMLButtonElement>('localAiClearBtn'),
    localAiTokenClearBtn: getRequiredElement<HTMLButtonElement>('localAiTokenClearBtn'),
    localAiSaveBtn: getRequiredElement<HTMLButtonElement>('localAiSaveBtn'),

    timezoneLocationCard: getRequiredElement<CardField>('timezoneLocationCard'),
    timezoneSelect: getRequiredElement<ValidatableField>('timezoneSelect'),
    manualTimeInput: getRequiredElement<ValidatableField>('manualTimeInput'),
    manualDateInput: getRequiredElement<ValidatableField>('manualDateInput'),
    timezoneLocationClearBtn: getRequiredElement<HTMLButtonElement>('timezoneLocationClearBtn'),
    timezoneLocationSaveBtn: getRequiredElement<HTMLButtonElement>('timezoneLocationSaveBtn'),
  };
}
