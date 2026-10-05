# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Match the experiment device API without admitting unbound publishers."""

import pytest

from flash_gui.openjii_client import OpenJIIClient, OpenJIIError
from tools.fleet_deploy.workbook_schedule import experiment_thing_names, restrict_to_experiment, WorkbookDeliveryError


BOUND = {"id": "bound", "thingName": "ambyte_00:11:22:33:44:55"}
OBSERVED = {"id": "observed", "thingName": "ambyte_00:11:22:33:44:66"}
BINDING = {"addedBy": "owner", "addedAt": "2026-09-24T00:00:00Z"}


def client_for(payload, status=200):
    client = OpenJIIClient.__new__(OpenJIIClient)
    client._request = lambda *_args: (status, payload)
    return client


def test_overview_targets_only_bound_registry_devices():
    # The current API combines bound devices and observed publishers. The
    # latter are not authority to install this experiment's workbook.
    client = client_for({
        "devices": [
            {"device": BOUND, "clientId": BOUND["thingName"], "binding": BINDING},
            {"device": OBSERVED, "clientId": OBSERVED["thingName"], "binding": None},
            {"device": None, "clientId": "ambyte_00:11:22:33:44:77", "binding": None},
        ],
        "window": {"from": "2026-09-23T00:00:00Z", "to": "2026-09-24T00:00:00Z"},
        "pipelineUnavailable": False,
    })
    assert client.list_experiment_devices("experiment") == [BOUND]
    names, skipped = experiment_thing_names(client, "experiment")
    assert names == [BOUND["thingName"]]
    assert skipped == []
    with pytest.raises(WorkbookDeliveryError, match="not bound"):
        restrict_to_experiment([OBSERVED["thingName"]], names)


def test_legacy_list_remains_supported():
    client = client_for([{"device": BOUND, **BINDING}])
    assert client.list_experiment_devices("experiment") == [BOUND]


@pytest.mark.parametrize("payload", [{"devices": []}, []])
def test_empty_devices(payload):
    assert client_for(payload).list_experiment_devices("experiment") == []


def test_overview_without_binding_does_not_authorize_install():
    assert client_for({"devices": [{"device": OBSERVED}]}).list_experiment_devices("experiment") == []


@pytest.mark.parametrize("payload", [{}, {"devices": None}, {"devices": {}}, None])
def test_invalid_success_shape_fails(payload):
    with pytest.raises(OpenJIIError, match="listing devices"):
        client_for(payload).list_experiment_devices("experiment")


def test_forbidden_still_reports_access_error():
    with pytest.raises(OpenJIIError, match="collaborator access"):
        client_for({"message": "Forbidden"}, 403).list_experiment_devices("experiment")
