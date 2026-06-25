#!/usr/bin/env python3
import sys, time, argparse

sys.path.insert(0, "/usr/local/lib/wifitree")
import wtdb


def resolve_one(token):
    matches = wtdb.find_by_name_or_mac(token)
    if not matches:
        print(f"No user found matching '{token}'")
        sys.exit(1)
    if len(matches) > 1:
        print(f"Multiple users match '{token}', use the MAC address instead:")
        for u in matches:
            print(f"  {u['mac']}  {u['name']}")
        sys.exit(1)
    return matches[0]


def fmt_ago(ts):
    if not ts:
        return "never"
    s = time.time() - ts
    if s < 60:
        return f"{int(s)}s ago"
    if s < 3600:
        return f"{int(s/60)}m ago"
    if s < 86400:
        return f"{s/3600:.1f}h ago"
    return f"{s/86400:.1f}d ago"


def cmd_list(args):
    users = wtdb.list_users()
    if not users:
        print("No users registered yet.")
        return
    print(f"{'MAC':<18}{'NAME':<16}{'STATUS':<10}{'USED/MB':<14}{'BW':<10}{'LAST CHECKIN':<14}")
    for u in users:
        full = wtdb.is_full_speed(u)
        status = "FULL" if full else "TRICKLE"
        if u["force_full_speed"]:
            status += "*"
        used = (u["bytes_used_month"] or 0) / 1024 / 1024
        quota = u["monthly_limit_mb"] or wtdb.DEFAULT_MONTHLY_MB
        bw = f"{u['bw_limit_mbit']}mbit" if u["bw_limit_mbit"] else "-"
        print(f"{u['mac']:<18}{(u['name'] or '')[:15]:<16}{status:<10}"
              f"{f'{used:.1f}/{quota:.0f}':<14}{bw:<10}{fmt_ago(u['last_checkin']):<14}")
    print("\n(* = admin override forcing full speed regardless of quota/checkin)")


def cmd_set_bw(args):
    u = resolve_one(args.user)
    wtdb.set_bw_limit(u["mac"], args.mbit)
    print(f"Set {u['name']} ({u['mac']}) bandwidth limit to {args.mbit} mbit"
          if args.mbit else f"Cleared custom bandwidth limit for {u['name']} ({u['mac']})")


def cmd_set_limit(args):
    u = resolve_one(args.user)
    wtdb.set_monthly_limit(u["mac"], args.mb)
    print(f"Set {u['name']} ({u['mac']}) monthly limit to {args.mb} MB")


def cmd_reset(args):
    u = resolve_one(args.user)
    wtdb.reset_usage(u["mac"])
    print(f"Reset monthly usage for {u['name']} ({u['mac']})")


def cmd_unblock(args):
    u = resolve_one(args.user)
    wtdb.set_force_full_speed(u["mac"], True)
    print(f"{u['name']} ({u['mac']}) forced to FULL speed regardless of quota/checkin")


def cmd_unforce(args):
    u = resolve_one(args.user)
    wtdb.set_force_full_speed(u["mac"], False)
    print(f"Removed forced full-speed override for {u['name']} ({u['mac']})")


def cmd_remove(args):
    u = resolve_one(args.user)
    wtdb.delete_user(u["mac"])
    print(f"Removed {u['name']} ({u['mac']}) entirely")


def main():
    p = argparse.ArgumentParser(prog="wifitree-admin")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="show all users and their status").set_defaults(func=cmd_list)

    sp = sub.add_parser("set-bw", help="set a custom bandwidth cap (mbit) for a user")
    sp.add_argument("user", help="name or MAC address")
    sp.add_argument("mbit", type=float, nargs="?", default=None,
                     help="bandwidth in mbit/s (omit to clear override)")
    sp.set_defaults(func=cmd_set_bw)

    sp = sub.add_parser("set-limit", help="set monthly data limit (MB) for a user")
    sp.add_argument("user")
    sp.add_argument("mb", type=float)
    sp.set_defaults(func=cmd_set_limit)

    sp = sub.add_parser("reset", help="reset a user's monthly usage counter")
    sp.add_argument("user")
    sp.set_defaults(func=cmd_reset)

    sp = sub.add_parser("unblock", help="force full speed regardless of quota/checkin")
    sp.add_argument("user")
    sp.set_defaults(func=cmd_unblock)

    sp = sub.add_parser("unforce", help="remove forced full-speed override")
    sp.add_argument("user")
    sp.set_defaults(func=cmd_unforce)

    sp = sub.add_parser("remove", help="delete a user's account entirely")
    sp.add_argument("user")
    sp.set_defaults(func=cmd_remove)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
