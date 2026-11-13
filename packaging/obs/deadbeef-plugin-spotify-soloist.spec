%global __builder ninja

Name:           deadbeef-plugin-spotify-soloist
Version:        0.1.0
Release:        0
Summary:        DeaDBeeF plugin bridging Spotify Web API search, XSPF playlists, and Spotify Soloist
License:        MIT
URL:            https://github.com/Kleyguerth/deadbeef-spotify-soloist-plugin
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++
BuildRequires:  ninja
BuildRequires:  pkgconfig
BuildRequires:  deadbeef-devel
BuildRequires:  libcurl-devel
BuildRequires:  nlohmann_json-devel
BuildRequires:  pugixml-devel
BuildRequires:  pkgconfig(gtk+-3.0)

Requires:       deadbeef

%description
A DeaDBeeF plugin that enables searching the Spotify catalog (tracks and artist
discographies), saving and loading XSPF playlists with spotify%3Atrack%3A...
locations, and controlling a local Spotify Soloist audio daemon via WebSocket.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake \
    -DBUILD_TESTS=ON
%cmake_build

%check
%ctest

%install
%cmake_install

%files
%{_libdir}/deadbeef/ddb_spotify_soloist.so
%{_libdir}/deadbeef/ddb_spotify_xspf.so
%{_libdir}/deadbeef/ddb_spotify_gtkui.so

%changelog
* Fri Nov 13 2026 Kleyguerth - 0.1.0-0
- Initial DeaDBeeF plugin package
