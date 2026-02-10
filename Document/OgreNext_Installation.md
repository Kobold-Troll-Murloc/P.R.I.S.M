# OGRE Next (Ogre 2.x) 설치 및 환경 구축 가이드

이 문서는 P.R.I.S.M 프로젝트 개발을 위한 OGRE Next 엔진 빌드 및 자동화 환경 설정 방법을 안내합니다.

---

## 🚀 1. 빠른 설치 (One-Click Setup)

수동 설정 없이 PowerShell 스크립트 하나로 엔진 빌드부터 의존성 설치까지 모든 과정을 자동화할 수 있습니다.

### 사전 준비 사항
*   **Visual Studio 2022**: 'C++를 사용한 데스크톱 개발' 워크로드 설치 필수
*   **CMake (3.10 이상)**: 시스템 PATH에 등록 필수
*   **Python 3.x**: 시스템 PATH에 등록 필수 (Vulkan 셰이더 빌드용)
*   **Git**: 소스 코드 관리용

### 자동 설치 단계
1.  **터미널 실행**: PowerShell을 관리자 권한으로 엽니다.
2.  **스크립트 실행**:
    ```powershell
    cd Project/1_JGN
    powershell -ExecutionPolicy Bypass -File .\setup.ps1
    ```
3.  **대기**: 약 15~30분 후 "Setup Completed Successfully!" 문구가 뜨면 완료됩니다.

---

## 📂 2. 주요 생성 결과물

스크립트 실행 후 생성되는 핵심 폴더들은 다음과 같습니다. 메인 프로젝트 빌드 시 이 경로들을 참조합니다.

*   **OGRE Next SDK**: `Project/1_JGN/ogre-next/build/sdk`
    *   엔진의 헤더(`include`), 라이브러리(`lib`), 바이너리(`bin`)가 포함되어 있습니다.
*   **의존성 라이브러리**: `Project/1_JGN/ogre-next-deps/build/ogredeps`
    *   SDL2, FreeImage, Freetype 등 필수 라이브러리가 포함되어 있습니다.

---

## 🛠️ 3. 메인 프로젝트 (P.R.I.S.M) 빌드 방법

환경 구축이 완료되었다면 아래 순서로 본 프로젝트를 빌드하고 실행할 수 있습니다.

1.  **프로젝트 열기**: `Project/1_JGN/P.R.I.S.M` 폴더를 IDE(VS Code, Visual Studio 등)로 엽니다.
2.  **CMake 구성 (Configure)**:
    *   CMake가 자동으로 `ogre-next/build/sdk` 경로를 찾아 엔진을 연결합니다.
3.  **빌드 (Build)**:
    *   빌드가 완료되면 **DLL 및 설정 파일(resources2.cfg 등)이 실행 폴더로 자동 복사**됩니다.
4.  **실행**:
    *   `build/bin/Release/PRISM.exe` (또는 Debug)를 실행하여 결과를 확인합니다.

---

## ⚠️ 4. 문제 해결 (Troubleshooting)

### 스크립트 실행 중 에러 발생 시
*   **FreeImage.h 미검출**: 스크립트 V3 이상 버전을 사용 중인지 확인하세요. `ogre-next-deps` 빌드가 먼저 완료되어야 합니다.
*   **권한 문제**: PowerShell을 반드시 **관리자 권한**으로 실행하고, `-ExecutionPolicy Bypass` 옵션을 잊지 마세요.
*   **경로 길이**: Windows의 경로 제한(260자) 문제 방지를 위해 프로젝트는 `D:\Git\PRISM`과 같이 가급적 짧은 경로에 두는 것이 좋습니다.

### 런타임 에러 (DLL 없음 등)
*   `P.R.I.S.M/CMakeLists.txt`의 `POST_BUILD` 로직이 정상 작동했는지 확인하세요. 
*   수동으로 복사할 경우 `sdk/bin`과 `ogredeps/bin`에 있는 DLL들을 실행 파일 위치로 복사하면 됩니다.
