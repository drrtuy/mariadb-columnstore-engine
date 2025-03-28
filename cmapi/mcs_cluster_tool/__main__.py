import logging
import subprocess
import sys

import typer

from cmapi_server.logging_management import dict_config, add_logging_level
from mcs_cluster_tool import (
    cluster_app, cmapi_app, backup_commands, restore_commands
)
from mcs_cluster_tool.constants import MCS_CLI_LOG_CONF_PATH


# don't show --install-completion and --show-completion options in help message
app = typer.Typer(
    add_completion=False,
    help=(
        'The  MCS  Command  Line  Interface is a unified tool to manage your '
        'MCS services'
    ),
    rich_markup_mode='rich',
)
app.add_typer(cluster_app.app)
# TODO: keep this only for potential backward compatibility
app.add_typer(cluster_app.app, name='cluster', hidden=True)
app.add_typer(cmapi_app.app, name='cmapi')
app.command('backup')(backup_commands.backup)
app.command('dbrm_backup')(backup_commands.dbrm_backup)
app.command('restore')(restore_commands.restore)
app.command('dbrm_restore')(restore_commands.dbrm_restore)


@app.command(
        name='help-all', help='Show help for all commands in man page style.',
        add_help_option=False
)
def help_all():
    # Open the man page in interactive mode
    subprocess.run(['man', 'mcs'])


if __name__ == '__main__':
    add_logging_level('TRACE', 5)  #TODO: remove when stadalone mode added.
    dict_config(MCS_CLI_LOG_CONF_PATH)
    logger = logging.getLogger('mcs_cli')
    # add separator between cli commands logging
    logger.debug(f'{"-":-^80}')
    cl_args_line = ' '.join(sys.argv[1:])
    logger.debug(f'Called "mcs {cl_args_line}"')
    app(prog_name='mcs')
