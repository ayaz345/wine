name	rasapi32
type	win32

import	kernel32.dll
import	ntdll.dll

debug_channels (ras)

500 stub	RnaEngineRequest
501 stub	DialEngineRequest
502 stub	SuprvRequest
503 stub	DialInMessage
504 stub	RnaEnumConnEntries
505 stub	RnaGetConnEntry
506 stub	RnaFreeConnEntry
507 stub	RnaSaveConnEntry
508 stub	RnaDeleteConnEntry
509 stub	RnaRenameConnEntry
510 stub	RnaValidateEntryName
511 stub	RnaEnumDevices
512 stub	RnaGetDeviceInfo
513 stub	RnaGetDefaultDevConfig
514 stub	RnaBuildDevConfig
515 stub	RnaDevConfigDlg
516 stub	RnaFreeDevConfig
517 stub	RnaActivateEngine
518 stub	RnaDeactivateEngine
519 stub	SuprvEnumAccessInfo
520 stub	SuprvGetAccessInfo
521 stub	SuprvSetAccessInfo
522 stub	SuprvGetAdminConfig
523 stub	SuprvInitialize
524 stub	SuprvDeInitialize
525 stub	RnaUIDial
526 stub	RnaImplicitDial
527 stub	RasDial16
528 stub	RnaSMMInfoDialog
529 stub	RnaEnumerateMacNames
530 stub	RnaEnumCountryInfo
531 stub	RnaGetAreaCodeList
532 stub	RnaFindDriver
533 stub	RnaInstallDriver
534 stub	RnaGetDialSettings
535 stub	RnaSetDialSettings
536 stub	RnaGetIPInfo
537 stub	RnaSetIPInfo
538 stub	RasCreatePhonebookEntryA
539 stub	RasCreatePhonebookEntryW
540 stub	RasDialA
541 stub	RasDialW
542 stub	RasEditPhonebookEntryA
543 stub	RasEditPhonebookEntryW
544 stdcall	RasEnumConnectionsA(ptr ptr ptr) RasEnumConnectionsA
545 stub	RasEnumConnectionsW
546 stdcall	RasEnumEntriesA(str str ptr ptr ptr) RasEnumEntriesA
547 stub	RasEnumEntriesW
548 stub	RasGetConnectStatusA
549 stub	RasGetConnectStatusW
550 stdcall	RasGetEntryDialParamsA(str ptr ptr) RasGetEntryDialParamsA
551 stub	RasGetEntryDialParamsW
552 stub	RasGetErrorStringA
553 stub	RasGetErrorStringW
554 stub	RasGetProjectionInfoA
555 stub	RasGetProjectionInfoW
556 stdcall	RasHangUpA(long) RasHangUpA
557 stub	RasHangUpW
558 stub	RasSetEntryDialParamsA
559 stub	RasSetEntryDialParamsW
560 stub	RnaCloseMac
561 stub	RnaComplete
562 stub	RnaGetDevicePort
563 stub	RnaGetUserProfile
564 stub	RnaOpenMac
565 stub	RnaSessInitialize
566 stub	RnaStartCallback
567 stub	RnaTerminate
568 stub	RnaUICallbackDialog
569 stub	RnaUIUsernamePassword
