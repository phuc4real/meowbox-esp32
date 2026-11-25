"""Windows Network List Manager over COM: the name and category (Public/Private) of each connected network.

Replaces Get-NetConnectionProfile: endpoint protection (Sophos Intercept X "BrowserAncestorPowershell" lockdown)
blocks powershell.exe started by the agent when the agent was launched from an installer downloaded in a browser."""
import ctypes
import logging
import winreg
from ctypes import HRESULT, POINTER, c_int, c_short, c_ulong

from comtypes import BSTR, CLSCTX_ALL, COMMETHOD, GUID, CoCreateInstance, IUnknown
from comtypes.automation import IDispatch

log = logging.getLogger("meowbox")
CLSID_NETWORK_LIST_MANAGER = GUID("{DCB00C01-570F-4A9B-8D69-199FDBA5723B}")
CATEGORY = {0: "Public", 1: "Private", 2: "DomainAuthenticated"}
ADAPTERS_KEY = r"SYSTEM\CurrentControlSet\Control\Network\{4D36E972-E325-11CE-BFC1-08002BE10318}"


class INetwork(IDispatch):
    _iid_ = GUID("{DCB00002-570F-4A9B-8D69-199FDBA5723B}")


class INetworkConnection(IDispatch):
    _iid_ = GUID("{DCB00005-570F-4A9B-8D69-199FDBA5723B}")


class IEnumNetworkConnections(IDispatch):
    _iid_ = GUID("{DCB00006-570F-4A9B-8D69-199FDBA5723B}")


class INetworkListManager(IDispatch):
    _iid_ = GUID("{DCB00000-570F-4A9B-8D69-199FDBA5723B}")


# vtables from netlistmgr.h, up to the last method used
INetwork._methods_ = [
    COMMETHOD([], HRESULT, "GetName", (["out"], POINTER(BSTR))),
    COMMETHOD([], HRESULT, "SetName", (["in"], BSTR)),
    COMMETHOD([], HRESULT, "GetDescription", (["out"], POINTER(BSTR))),
    COMMETHOD([], HRESULT, "SetDescription", (["in"], BSTR)),
    COMMETHOD([], HRESULT, "GetNetworkId", (["out"], POINTER(GUID))),
    COMMETHOD([], HRESULT, "GetDomainType", (["out"], POINTER(c_int))),
    COMMETHOD([], HRESULT, "GetNetworkConnections", (["out"], POINTER(POINTER(IEnumNetworkConnections)))),
    COMMETHOD([], HRESULT, "GetTimeCreatedAndConnected", (["out"], POINTER(c_ulong)), (["out"], POINTER(c_ulong)),
              (["out"], POINTER(c_ulong)), (["out"], POINTER(c_ulong))),
    COMMETHOD([], HRESULT, "get_IsConnectedToInternet", (["out"], POINTER(c_short))),
    COMMETHOD([], HRESULT, "get_IsConnected", (["out"], POINTER(c_short))),
    COMMETHOD([], HRESULT, "GetConnectivity", (["out"], POINTER(c_int))),
    COMMETHOD([], HRESULT, "GetCategory", (["out"], POINTER(c_int))),
]
INetworkConnection._methods_ = [
    COMMETHOD([], HRESULT, "GetNetwork", (["out"], POINTER(POINTER(INetwork)))),
    COMMETHOD([], HRESULT, "get_IsConnectedToInternet", (["out"], POINTER(c_short))),
    COMMETHOD([], HRESULT, "get_IsConnected", (["out"], POINTER(c_short))),
    COMMETHOD([], HRESULT, "GetConnectivity", (["out"], POINTER(c_int))),
    COMMETHOD([], HRESULT, "GetConnectionId", (["out"], POINTER(GUID))),
    COMMETHOD([], HRESULT, "GetAdapterId", (["out"], POINTER(GUID))),
]
IEnumNetworkConnections._methods_ = [
    COMMETHOD([], HRESULT, "get__NewEnum", (["out"], POINTER(POINTER(IUnknown)))),
    COMMETHOD([], HRESULT, "Next", (["in"], c_ulong), (["out"], POINTER(POINTER(INetworkConnection))),
              (["out"], POINTER(c_ulong))),
]
INetworkListManager._methods_ = [
    COMMETHOD([], HRESULT, "GetNetworks", (["in"], c_int), (["out"], POINTER(POINTER(IUnknown)))),
    COMMETHOD([], HRESULT, "GetNetwork", (["in"], GUID), (["out"], POINTER(POINTER(INetwork)))),
    COMMETHOD([], HRESULT, "GetNetworkConnections", (["out"], POINTER(POINTER(IEnumNetworkConnections)))),
]


def _alias(adapter_id: str) -> str:
    """Adapter GUID -> interface alias ("Wi-Fi", "Ethernet"), the names psutil.net_if_addrs() uses."""
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, rf"{ADAPTERS_KEY}\{adapter_id}\Connection") as k:
            return winreg.QueryValueEx(k, "Name")[0]
    except OSError:
        return ""


def _query() -> list[tuple[str, str, str]]:
    nlm = CoCreateInstance(CLSID_NETWORK_LIST_MANAGER, interface=INetworkListManager, clsctx=CLSCTX_ALL)
    conns = nlm.GetNetworkConnections()
    out = []
    while True:
        conn, fetched = conns.Next(1)
        if not fetched:
            break
        net = conn.GetNetwork()
        out.append((_alias(str(conn.GetAdapterId())), net.GetName(), CATEGORY.get(net.GetCategory(), "")))
    return out  # COM pointers are released here, before the caller uninitializes COM


def connections() -> list[tuple[str, str, str]]:
    """(interface alias, network name, category) per connected adapter; [] if the query fails.
    Safe from any thread: initializes COM for the call."""
    hr = ctypes.windll.ole32.CoInitializeEx(None, 2)  # COINIT_APARTMENTTHREADED; S_FALSE if already initialized
    try:
        return _query()
    except Exception as e:  # noqa: BLE001
        log.debug("network list query failed: %s", e)
        return []
    finally:
        if hr >= 0:
            ctypes.windll.ole32.CoUninitialize()
