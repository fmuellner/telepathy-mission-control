# Copyright (C) 2009-2010 Nokia Corporation
# Copyright (C) 2009-2010 Collabora Ltd.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
# 02110-1301 USA

import time
import os
import os.path
import signal

import dbus
import dbus.service

from servicetest import EventPattern, tp_name_prefix, tp_path_prefix, \
        call_async
from mctest import (
    exec_test, create_fakecm_account, get_fakecm_account, connect_to_mc,
    keyfile_read, tell_mc_to_die, resuscitate_mc
    )
import constants as cs

use_keyring = False
if ('MC_TEST_GNOME_KEYRING' in os.environ and
    os.environ['MC_TEST_GNOME_KEYRING'] == '1'):
        use_keyring = True

def create_keyring():
    if not use_keyring:
        return

    keyring = os.popen('../keyring-command create').read()
    if not keyring or keyring.startswith('**'):
        return None
    return keyring[:-1]

def remove_keyring(name):
    if not use_keyring:
        return

    os.system('../keyring-command remove ' + name)

def account_store(op, backend, key=None, value=None):
    cmd = [ '../account-store', op, backend,
        'fakecm/fakeprotocol/dontdivert_40example_2ecom0' ]
    if key:
        cmd.append(key)
        if value:
            cmd.append(value)

    lines = os.popen(' '.join(cmd)).read()
    ret = []
    for line in lines.split('\n'):
        if line.startswith('** '):
            continue

        if line:
            ret.append(line)

    if len(ret) > 0:
        return ret[0]
    else:
        return None

def start_gnome_keyring_daemon(ctl_dir):
    if not use_keyring:
        return

    os.chmod(ctl_dir, 0700)
    env = os.popen('gnome-keyring-daemon -d --control-directory=' + ctl_dir).read()
    env_file = open(ctl_dir + '/gnome-keyring-env', 'w')

    for line in env.split('\n'):
        if line:
            k, v = line.split('=', 1)
            print "Adding to env: %s=%s" % (k, v)
            os.environ[k] = v
            env_file.write('%s=%s\n' % (k, v))

    keyring_name = create_keyring()
    assert keyring_name
    print "Created new keyring name, putting to env", keyring_name
    os.environ['MC_KEYRING_NAME'] = keyring_name
    env_file.write('MC_KEYRING_NAME=%s\n' % keyring_name)
    env_file.close()

def stop_gnome_keyring_daemon():
    if not use_keyring:
        return

    keyring_name = os.environ['MC_KEYRING_NAME']
    keyring_daemon_pid = os.environ['GNOME_KEYRING_PID']

    if keyring_name:
        print "Removing keyring", keyring_name
        remove_keyring(keyring_name)

    if keyring_daemon_pid:
        print "Killing keyring daemon, pid =", keyring_daemon_pid
        os.kill(int(keyring_daemon_pid), 15)

def test(q, bus, mc):
    ctl_dir = os.environ['MC_ACCOUNT_DIR']
    key_file_name = os.path.join(ctl_dir, 'accounts.cfg')
    group = 'fakecm/fakeprotocol/dontdivert_40example_2ecom0'

    account_manager, properties, interfaces = connect_to_mc(q, bus, mc)

    assert properties.get('ValidAccounts') == [], \
        properties.get('ValidAccounts')
    assert properties.get('InvalidAccounts') == [], \
        properties.get('InvalidAccounts')

    params = dbus.Dictionary({"account": "dontdivert@example.com",
        "password": "secrecy"}, signature='sv')
    (cm_name_ref, account) = create_fakecm_account(q, bus, mc, params)

    account_path = account.__dbus_object_path__

    # Check the account is correctly created
    properties = account_manager.GetAll(cs.AM,
            dbus_interface=cs.PROPERTIES_IFACE)
    assert properties is not None
    assert properties.get('ValidAccounts') == [account_path], properties
    account_path = properties['ValidAccounts'][0]
    assert isinstance(account_path, dbus.ObjectPath), repr(account_path)
    assert properties.get('InvalidAccounts') == [], properties

    account_iface = dbus.Interface(account, cs.ACCOUNT)
    account_props = dbus.Interface(account, cs.PROPERTIES_IFACE)

    # Alter some miscellaneous r/w properties

    account_props.Set(cs.ACCOUNT, 'DisplayName', 'Work account')
    account_props.Set(cs.ACCOUNT, 'Icon', 'im-jabber')
    account_props.Set(cs.ACCOUNT, 'Nickname', 'Joe Bloggs')

    tell_mc_to_die(q, bus)

    # .. let's check the keyfile
    kf = keyfile_read(key_file_name)
    assert group in kf, kf
    assert kf[group]['manager'] == 'fakecm'
    assert kf[group]['protocol'] == 'fakeprotocol'
    assert kf[group]['param-account'] == params['account'], kf
    assert kf[group]['DisplayName'] == 'Work account', kf
    assert kf[group]['Icon'] == 'im-jabber', kf
    assert kf[group]['Nickname'] == 'Joe Bloggs', kf

    # This works wherever the password is stored
    pwd = account_store('get', 'default', 'param-password')
    assert pwd == params['password'], pwd

    # If we're using GNOME keyring, the password should not be in the
    # password file
    if use_keyring:
        assert 'param-password' not in kf[group]
    else:
        assert kf[group]['param-password'] == params['password'], kf

    # Reactivate MC
    account_manager, properties, interfaces = resuscitate_mc(q, bus, mc)
    account = get_fakecm_account(bus, mc, account_path)
    account_iface = dbus.Interface(account, cs.ACCOUNT)

    # Delete the account
    assert account_iface.Remove() is None
    account_event, account_manager_event = q.expect_many(
        EventPattern('dbus-signal',
            path=account_path,
            signal='Removed',
            interface=cs.ACCOUNT,
            args=[]
            ),
        EventPattern('dbus-signal',
            path=cs.AM_PATH,
            signal='AccountRemoved',
            interface=cs.AM,
            args=[account_path]
            ),
        )

    # Check the account is correctly deleted
    kf = keyfile_read(key_file_name)
    assert group not in kf, kf


if __name__ == '__main__':
    ctl_dir = os.environ['MC_ACCOUNT_DIR']
    start_gnome_keyring_daemon(ctl_dir)
    exec_test(test, {}, timeout=10)
    stop_gnome_keyring_daemon()
